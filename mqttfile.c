/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * The client half of file2mqtt: list, pull and push files across MQTT.
 *
 *	mqttfile [-t seconds] <broker_ip> <topic> list
 *	mqttfile [-t seconds] <broker_ip> <topic> get <remote> [local]
 *	mqttfile [-t seconds] <broker_ip> <topic> put <local> [remote]
 *
 * -t is how long to wait for a reply before giving up (default 20).
 *
 * The protocol is in filemqtt.h.  Chunks go at QoS 0, so a push ends with a
 * `missing` / resend loop until the bridge has every chunk; the bridge then
 * checks the CRC before putting the file in place.  A pull that comes up short
 * is simply retried whole - there is no per-chunk repair in that direction,
 * because in practice a local broker over TCP does not lose them.
 */
#include "filemqtt.h"
#include "smolmqtt.h"

#ifndef NOLIBC
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <fcntl.h>
#include <poll.h>
#endif

#define GET_RETRIES	3

struct client {
	struct smolmqtt m;
	char t_req[128], t_rsp[128], t_in[128], t_out[128];
	int timeout;

	/* what the callback collected */
	char line[FILEMQTT_LINE];	/* last /rsp message		*/
	int have_line;
	int fd;				/* destination of a pull	*/
	uint8_t *map;			/* chunks received in a pull	*/
	uint32_t chunks;
	uint32_t got;
};

static void pub_req(struct client *c, const char *s)
{
	smolmqtt_publish(&c->m, c->t_req, s, (uint32_t) strlen(s), 0, false);
}

static void on_msg(struct smolmqtt *m, const struct smolmqtt_message *msg,
		   void *priv)
{
	struct client *c = priv;

	(void) m;

	if (msg->topic_len == strlen(c->t_rsp) &&
	    !strncmp(msg->topic, c->t_rsp, msg->topic_len)) {
		uint32_t n = msg->payload_len;

		if (n > sizeof(c->line) - 1)
			n = sizeof(c->line) - 1;
		memcpy(c->line, msg->payload, n);
		c->line[n] = 0;
		c->have_line = 1;
		return;
	}

	if (msg->topic_len == strlen(c->t_out) &&
	    !strncmp(msg->topic, c->t_out, msg->topic_len)) {
		uint32_t idx, n;

		if (c->fd < 0 || msg->payload_len < FILEMQTT_HDR)
			return;
		idx = filemqtt_be32(msg->payload);
		n = msg->payload_len - FILEMQTT_HDR;
		if (lseek(c->fd, (off_t) idx * FILEMQTT_CHUNK, SEEK_SET) < 0)
			return;
		if (write(c->fd, msg->payload + FILEMQTT_HDR, n) != (ssize_t) n)
			return;
		if (c->map && idx < c->chunks && !filemqtt_bit_get(c->map, idx)) {
			filemqtt_bit_set(c->map, idx);
			c->got++;
		}
	}
}

/* Pump the connection until a /rsp message arrives, or the timeout expires.
 * Returns the line, or NULL.
 */
static const char *wait_line(struct client *c)
{
	int waited = 0;

	c->have_line = 0;
	while (waited < c->timeout * 1000) {
		struct pollfd p = { .fd = c->m.fd, .events = POLLIN };
		int r = poll(&p, 1, 200);

		if (r < 0)
			return NULL;
		if (r > 0 && smolmqtt_poll(&c->m, on_msg, c))
			return NULL;
		if (c->have_line)
			return c->line;
		if (!r)
			waited += 200;
	}

	return NULL;
}

/* Drain whatever is already queued, without waiting for a /rsp line. */
static void pump(struct client *c, int ms)
{
	int waited = 0;

	while (waited < ms) {
		struct pollfd p = { .fd = c->m.fd, .events = POLLIN };
		int r = poll(&p, 1, 50);

		if (r < 0)
			return;
		if (r > 0) {
			if (smolmqtt_poll(&c->m, on_msg, c))
				return;
			continue;
		}
		waited += 50;
	}
}

static int is_err(const char *line)
{
	return !strncmp(line, "err", 3);
}

/* ---- commands ----------------------------------------------------------- */

static int do_list(struct client *c)
{
	pub_req(c, "list");

	for (;;) {
		const char *line = wait_line(c);

		if (!line) {
			printf("timed out\n");
			return 1;
		}
		printf("%s\n", line);
		if (!strncmp(line, "ok ", 3))
			return 0;
		if (is_err(line))
			return 1;
	}
}

static int do_get(struct client *c, const char *remote, const char *local)
{
	char cmd[FILEMQTT_LINE];
	int attempt;

	for (attempt = 0; attempt < GET_RETRIES; attempt++) {
		uint32_t size = 0, crc = 0, chunks = 0, mycrc = 0, total = 0;
		uint8_t buf[FILEMQTT_CHUNK];
		const char *line, *p;
		char word[FILEMQTT_NAME_MAX];

		c->fd = open(local, O_RDWR | O_CREAT | O_TRUNC, 0644);
		if (c->fd < 0) {
			printf("cannot create %s\n", local);
			return 1;
		}
		c->chunks = 0;
		c->got = 0;
		free(c->map);
		c->map = NULL;

		snprintf(cmd, sizeof(cmd), "get %s", remote);
		pub_req(c, cmd);

		line = wait_line(c);
		if (!line) {
			printf("timed out waiting for the bridge\n");
			close(c->fd);
			c->fd = -1;
			return 1;
		}
		if (is_err(line)) {
			printf("%s\n", line);
			close(c->fd);
			c->fd = -1;
			return 1;
		}

		/* "ok get <name> <size> <crc> <chunks>" */
		p = line;
		filemqtt_word(&p, word, sizeof(word));	/* ok    */
		filemqtt_word(&p, word, sizeof(word));	/* get   */
		filemqtt_word(&p, word, sizeof(word));	/* name  */
		size = filemqtt_num(&p, 0);
		crc = filemqtt_num(&p, 1);
		chunks = filemqtt_num(&p, 0);

		/* Chunks are published before the reply, so they are almost
		 * certainly already in the socket; give the tail a moment.
		 */
		pump(c, 500);

		/* O_TRUNC plus writes at chunk offsets leave the file exactly
		 * as long as the last chunk reaches, so no truncate is needed.
		 */
		if (lseek(c->fd, 0, SEEK_SET) < 0) {
			close(c->fd);
			c->fd = -1;
			return 1;
		}
		for (;;) {
			ssize_t n = read(c->fd, buf, sizeof(buf));

			if (n <= 0)
				break;
			mycrc = filemqtt_crc32(mycrc, buf, (uint32_t) n);
			total += (uint32_t) n;
		}
		close(c->fd);
		c->fd = -1;

		if (total == size && mycrc == crc) {
			printf("got %s -> %s: %u bytes in %u chunks, crc %08x\n",
			       remote, local, total, chunks, crc);
			return 0;
		}

		printf("transfer incomplete (%u/%u bytes, crc %08x vs %08x)%s\n",
		       total, size, mycrc, crc,
		       attempt + 1 < GET_RETRIES ? ", retrying" : "");
	}

	return 1;
}

static int do_put(struct client *c, const char *local, const char *remote)
{
	uint8_t chunk[FILEMQTT_HDR + FILEMQTT_CHUNK];
	char cmd[FILEMQTT_LINE], word[FILEMQTT_NAME_MAX];
	uint32_t size = 0, crc = 0, idx, round;
	const char *line, *p;
	int fd;

	fd = open(local, O_RDONLY);
	if (fd < 0) {
		printf("cannot open %s\n", local);
		return 1;
	}
	for (;;) {
		ssize_t n = read(fd, chunk, FILEMQTT_CHUNK);

		if (n <= 0)
			break;
		crc = filemqtt_crc32(crc, chunk, (uint32_t) n);
		size += (uint32_t) n;
	}

	snprintf(cmd, sizeof(cmd), "put %s %u %08x", remote, size, crc);
	pub_req(c, cmd);

	line = wait_line(c);
	if (!line) {
		printf("timed out waiting for the bridge\n");
		close(fd);
		return 1;
	}
	if (is_err(line)) {
		printf("%s\n", line);
		close(fd);
		return 1;
	}
	printf("pushing %s as %s: %u bytes, crc %08x\n", local, remote, size,
	       crc);

	/* Send every chunk once... */
	idx = 0;
	if (lseek(fd, 0, SEEK_SET) < 0) {
		close(fd);
		return 1;
	}
	for (;;) {
		ssize_t n = read(fd, chunk + FILEMQTT_HDR, FILEMQTT_CHUNK);

		if (n <= 0)
			break;
		filemqtt_put_be32(chunk, idx++);
		if (smolmqtt_publish(&c->m, c->t_in, chunk,
				     FILEMQTT_HDR + (uint32_t) n, 0, false)) {
			printf("publish failed at chunk %u\n", idx - 1);
			close(fd);
			return 1;
		}
	}

	/* ...then repair whatever did not arrive. */
	for (round = 0; round < 32; round++) {
		uint32_t resent = 0;

		pub_req(c, "missing");
		line = wait_line(c);
		if (!line) {
			printf("timed out asking for missing chunks\n");
			close(fd);
			return 1;
		}
		if (is_err(line)) {
			printf("%s\n", line);
			close(fd);
			return 1;
		}

		p = line;
		filemqtt_word(&p, word, sizeof(word));	/* "missing" */
		filemqtt_word(&p, word, sizeof(word));
		if (!strcmp(word, "none"))
			break;

		/* re-walk the list, re-reading each wanted chunk */
		p = line + 8;
		for (;;) {
			uint32_t want;
			ssize_t n;

			while (*p == ' ')
				p++;
			if (*p < '0' || *p > '9')
				break;
			want = filemqtt_num(&p, 0);

			if (lseek(fd, (off_t) want * FILEMQTT_CHUNK,
				  SEEK_SET) < 0)
				break;
			n = read(fd, chunk + FILEMQTT_HDR, FILEMQTT_CHUNK);
			if (n <= 0)
				break;
			filemqtt_put_be32(chunk, want);
			smolmqtt_publish(&c->m, c->t_in, chunk,
					 FILEMQTT_HDR + (uint32_t) n, 0, false);
			resent++;
		}
		printf("resent %u chunk%s\n", resent, resent == 1 ? "" : "s");
		if (!resent)
			break;
	}

	close(fd);

	pub_req(c, "end");
	line = wait_line(c);
	if (!line) {
		printf("timed out waiting for the commit\n");
		return 1;
	}
	printf("%s\n", line);

	return is_err(line) ? 1 : 0;
}

static void usage(const char *argv0)
{
	printf("usage: %s [-t seconds] <broker_ip> <topic> list\n", argv0);
	printf("       %s [-t seconds] <broker_ip> <topic> get <remote> [local]\n",
	       argv0);
	printf("       %s [-t seconds] <broker_ip> <topic> put <local> [remote]\n",
	       argv0);
}

static const char *basename_of(const char *path)
{
	const char *p = path, *last = path;

	while (*p) {
		if (*p == '/')
			last = p + 1;
		p++;
	}

	return last;
}

int main(int argc, char **argv)
{
	struct client c = { 0 };
	const char *broker, *base, *op;
	char clientid[64];
	int ret, opt, rc;

	c.timeout = 20;
	c.fd = -1;

	while ((opt = getopt(argc, argv, "t:")) != -1) {
		switch (opt) {
		case 't':
			c.timeout = atoi(optarg);
			break;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	if (argc - optind < 3) {
		usage(argv[0]);
		return 1;
	}
	broker = argv[optind];
	base = argv[optind + 1];
	op = argv[optind + 2];

	snprintf(c.t_req, sizeof(c.t_req), "%s/req", base);
	snprintf(c.t_rsp, sizeof(c.t_rsp), "%s/rsp", base);
	snprintf(c.t_in, sizeof(c.t_in), "%s/in", base);
	snprintf(c.t_out, sizeof(c.t_out), "%s/out", base);
	snprintf(clientid, sizeof(clientid), "mqttfile-%d", (int) getpid());

	ret = smolmqtt_connect(&c.m, broker, 1883, clientid);
	if (ret) {
		printf("connect failed: %d\n", ret);
		return 1;
	}

	ret = smolmqtt_subscribe(&c.m, c.t_rsp, 0);
	if (!ret)
		ret = smolmqtt_subscribe(&c.m, c.t_out, 0);
	if (ret) {
		printf("subscribe failed: %d\n", ret);
		return 1;
	}

	if (!strcmp(op, "list")) {
		rc = do_list(&c);
	} else if (!strcmp(op, "get") && argc - optind >= 4) {
		const char *remote = argv[optind + 3];
		const char *local = argc - optind >= 5 ? argv[optind + 4] :
				    basename_of(remote);

		rc = do_get(&c, remote, local);
	} else if (!strcmp(op, "put") && argc - optind >= 4) {
		const char *local = argv[optind + 3];
		const char *remote = argc - optind >= 5 ? argv[optind + 4] :
				     basename_of(local);

		rc = do_put(&c, local, remote);
	} else {
		usage(argv[0]);
		rc = 1;
	}

	smolmqtt_disconnect(&c.m);
	free(c.map);

	return rc;
}
