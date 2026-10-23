/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Bridge a directory to MQTT: files can be pulled from it and pushed into it
 * over a single outbound MQTT connection.
 *
 *	file2mqtt [-r] [-s maxsize] <directory> <broker_ip> <topic>
 *
 * -r makes it read-only (pulls work, pushes are refused) and -s caps the size
 * of a push, in bytes or with a K, M or G on the end (default 32M).  Files
 * are addressed by bare name; any name containing a slash, or starting with
 * a dot, is refused, so nothing outside <directory> can be reached.
 *
 * The protocol lives in filemqtt.h.  Nothing is buffered whole: chunks are
 * written straight to <name>.part at their offset, and the finished file is
 * only renamed into place once its size and CRC match what the sender
 * promised.
 */
#include "filemqtt.h"
#include "smolmqtt.h"

#ifndef NOLIBC
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>
#endif

#include "config.h"
#include "inc/printing.h"
#include "inc/numbers.h"
#include "inc/strings.h"
#include "inc/paths.h"
#include "inc/fs.h"

#define USAGE	"usage: %s [-r] [-s maxsize] <directory> <broker_ip> <topic>\n"

#define DEFAULT_MAX_SIZE	(32u * 1024 * 1024)

struct bridge {
	struct smolmqtt *m;
	const char *dir;
	const char *topic_rsp;
	const char *topic_out;
	uint32_t max_size;
	int read_only;

	/* the push in progress, if any */
	int fd;
	char name[FILEMQTT_NAME_MAX];
	char part[FILEMQTT_LINE];
	uint32_t size;
	uint32_t crc;
	uint32_t chunks;
	uint8_t *map;
	int werr;			/* errno of the first failed write */
};

static void say(struct bridge *b, const char *s)
{
	smolmqtt_publish(b->m, b->topic_rsp, s, strlen(s), 0, false);
}

static void sayf(struct bridge *b, const char *fmt, ...)
{
	char out[FILEMQTT_LINE];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(out, sizeof(out), fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	if ((size_t) n >= sizeof(out))
		n = sizeof(out) - 1;

	smolmqtt_publish(b->m, b->topic_rsp, out, n, 0, false);
}

/* A bare name that stays inside the directory and fits where it is kept */
static bool name_ok(const char *n)
{
	return n[0] && n[0] != '.' && !strchr(n, '/') &&
	       strlen(n) < FILEMQTT_NAME_MAX;
}

/* Drop the in-progress push.  `keep_part` is set only on the success path,
 * where the .part file has already been renamed into place; otherwise it is
 * removed, so a failed or abandoned transfer leaves nothing behind in the
 * served directory.
 */
static void push_done(struct bridge *b, int keep_part)
{
	if (b->fd >= 0)
		close(b->fd);
	if (!keep_part && b->part[0])
		unlink(b->part);
	b->fd = -1;
	b->part[0] = 0;
	b->size = 0;
	b->chunks = 0;
	b->werr = 0;
	if (b->map) {
		free(b->map);
		b->map = NULL;
	}
}

static void push_reset(struct bridge *b)
{
	push_done(b, 0);
}

/* ---- commands ----------------------------------------------------------- */

struct listing {
	struct bridge *b;
	uint32_t count;
};

static int list_one(const char *name, int dirfd, void *priv)
{
	struct listing *l = priv;
	struct stat st;

	if (name[0] == '.')
		return 0;
	if (fstatat(dirfd, name, &st, 0) || !S_ISREG(st.st_mode))
		return 0;

	sayf(l->b, "file %s %u", name, (uint32_t) st.st_size);
	l->count++;

	return 0;
}

static void cmd_list(struct bridge *b)
{
	struct listing l = { .b = b };

	if (iterate_dir(b->dir, list_one, &l) < 0) {
		say(b, "err cannot open directory");
		return;
	}

	sayf(b, "ok list %u", l.count);
}

static void cmd_get(struct bridge *b, const char *name)
{
	char path[FILEMQTT_LINE];
	uint8_t chunk[FILEMQTT_HDR + FILEMQTT_CHUNK];
	uint32_t crc = 0, total = 0, idx = 0;
	struct stat st;
	int fd;

	if (!name_ok(name) || !path_join(path, sizeof(path), b->dir, name)) {
		say(b, "err bad name");
		return;
	}
	if (stat(path, &st) || !S_ISREG(st.st_mode)) {
		say(b, "err no such file");
		return;
	}

	fd = open(path, O_RDONLY);
	if (fd < 0) {
		say(b, "err cannot open");
		return;
	}

	for (;;) {
		ssize_t n = read(fd, chunk + FILEMQTT_HDR, FILEMQTT_CHUNK);

		if (n < 0) {
			close(fd);
			say(b, "err read failed");
			return;
		}
		if (!n)
			break;
		filemqtt_put_be32(chunk, idx++);
		crc = filemqtt_crc32(crc, chunk + FILEMQTT_HDR, (uint32_t) n);
		total += (uint32_t) n;
		if (smolmqtt_publish(b->m, b->topic_out, chunk,
				     FILEMQTT_HDR + (uint32_t) n, 0, false)) {
			close(fd);
			say(b, "err publish failed");
			return;
		}
	}

	close(fd);
	sayf(b, "ok get %s %u %08x %u", name, total, crc, idx);
}

static void cmd_put(struct bridge *b, const char *name, uint32_t size,
		    uint32_t crc)
{
	push_reset(b);

	if (b->read_only) {
		say(b, "err read-only");
		return;
	}
	if (!name_ok(name)) {
		say(b, "err bad name");
		return;
	}
	if (!size || size > b->max_size) {
		sayf(b, "err size out of range (max %u)", b->max_size);
		return;
	}

	strcpy(b->name, name);
	if ((size_t) snprintf(b->part, sizeof(b->part), "%s/%s.part", b->dir,
			      b->name) >= sizeof(b->part)) {
		b->part[0] = 0;
		say(b, "err bad name");
		return;
	}

	b->size = size;
	b->crc = crc;
	b->chunks = filemqtt_chunks(size);
	b->map = calloc(filemqtt_map_bytes(b->chunks), 1);
	if (!b->map) {
		say(b, "err out of memory");
		return;
	}

	b->fd = open(b->part, O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (b->fd < 0) {
		push_reset(b);
		say(b, "err cannot create .part file");
		return;
	}

	printf("put %s: %u bytes, %u chunks\n", b->name, size, b->chunks);
	sayf(b, "ok put ready %u %u", b->chunks, (uint32_t) FILEMQTT_CHUNK);
}

static void cmd_missing(struct bridge *b)
{
	char out[FILEMQTT_LINE];
	uint32_t i, listed = 0;
	size_t o;

	if (b->fd < 0) {
		say(b, "err no transfer in progress");
		return;
	}

	if (b->werr) {
		sayf(b, "err cannot write the chunks: %s", strerror(b->werr));
		return;
	}

	o = snprintf(out, sizeof(out), "missing");
	for (i = 0; i < b->chunks && listed < FILEMQTT_MAX_MISSING; i++) {
		if (filemqtt_bit_get(b->map, i))
			continue;
		/* Room for a space and the longest index */
		if (o + 12 > sizeof(out))
			break;
		o += snprintf(out + o, sizeof(out) - o, " %u", i);
		listed++;
	}

	if (!listed) {
		say(b, "missing none");
		return;
	}

	smolmqtt_publish(b->m, b->topic_rsp, out, o, 0, false);
}

static void cmd_end(struct bridge *b)
{
	uint8_t buf[FILEMQTT_CHUNK];
	char path[FILEMQTT_LINE];
	uint32_t crc = 0, total = 0, i;

	if (b->fd < 0) {
		say(b, "err no transfer in progress");
		return;
	}

	if (b->werr) {
		sayf(b, "err cannot write the chunks: %s", strerror(b->werr));
		return;
	}

	for (i = 0; i < b->chunks; i++) {
		if (!filemqtt_bit_get(b->map, i)) {
			say(b, "err incomplete, ask `missing`");
			return;
		}
	}

	if (lseek(b->fd, 0, SEEK_SET) < 0) {
		push_reset(b);
		say(b, "err seek failed");
		return;
	}
	for (;;) {
		ssize_t n = read(b->fd, buf, sizeof(buf));

		if (n <= 0)
			break;
		crc = filemqtt_crc32(crc, buf, (uint32_t) n);
		total += (uint32_t) n;
	}

	if (total != b->size || crc != b->crc) {
		sayf(b, "err mismatch: got %u bytes crc %08x, wanted %u %08x",
		     total, crc, b->size, b->crc);
		error("put %s FAILED: got %u/%08x wanted %u/%08x\n", b->name,
		      total, crc, b->size, b->crc);
		push_reset(b);
		return;
	}

	close(b->fd);
	b->fd = -1;

	if (!path_join(path, sizeof(path), b->dir, b->name) ||
	    rename(b->part, path)) {
		say(b, "err rename failed");
		push_reset(b);
		return;
	}

	printf("put %s: complete, %u bytes, crc %08x\n", b->name, total, crc);
	sayf(b, "ok put %s %u %08x", b->name, total, crc);
	push_done(b, 1);
}

/* ---- dispatch ----------------------------------------------------------- */

static void on_req(struct bridge *b, const uint8_t *payload, uint32_t len)
{
	char line[FILEMQTT_LINE];
	char *p = line, *cmd, *name, *end;

	if (len > sizeof(line) - 1)
		len = sizeof(line) - 1;
	memcpy(line, payload, len);
	line[len] = 0;

	/* One line of request; a line end and anything after it is not it */
	end = strchr(line, '\r');
	if (end)
		*end = 0;
	end = strchr(line, '\n');
	if (end)
		*end = 0;

	cmd = next_field(&p);
	if (!cmd)
		return;

	if (!strcmp(cmd, "list")) {
		cmd_list(b);
	} else if (!strcmp(cmd, "get")) {
		name = next_field(&p);
		if (!name) {
			say(b, "err bad name");
			return;
		}
		cmd_get(b, name);
	} else if (!strcmp(cmd, "put")) {
		uint32_t size, crc;

		name = next_field(&p);
		if (!name || !filemqtt_next_number(&p, 10, &size) ||
		    !filemqtt_next_number(&p, 16, &crc)) {
			say(b, "err put wants <name> <size> <crc32>");
			return;
		}
		cmd_put(b, name, size, crc);
	} else if (!strcmp(cmd, "missing")) {
		cmd_missing(b);
	} else if (!strcmp(cmd, "end")) {
		cmd_end(b);
	} else if (!strcmp(cmd, "abort")) {
		push_reset(b);
		say(b, "ok abort");
	} else {
		sayf(b, "err unknown command %s", cmd);
	}
}

static void on_chunk(struct bridge *b, const uint8_t *payload, uint32_t len)
{
	uint32_t idx, n;
	off_t off;

	if (b->fd < 0 || len < FILEMQTT_HDR)
		return;

	idx = get_unaligned_be32(payload);
	n = len - FILEMQTT_HDR;
	if (idx >= b->chunks || n > FILEMQTT_CHUNK)
		return;

	off = (off_t) idx * FILEMQTT_CHUNK;
	if (lseek(b->fd, off, SEEK_SET) < 0) {
		if (!b->werr)
			b->werr = errno ? errno : EIO;
		return;
	}
	if (write(b->fd, payload + FILEMQTT_HDR, n) != (ssize_t) n) {
		/*
		 * Keep the errno: a chunk that cannot be written otherwise
		 * looks like a lost one and is resent for ever.
		 */
		if (!b->werr)
			b->werr = errno ? errno : EIO;
		return;
	}

	filemqtt_bit_set(b->map, idx);
}

struct ctx {
	struct bridge *b;
	const char *topic_req;
	const char *topic_in;
};

static void on_mqtt(struct smolmqtt *m, const struct smolmqtt_message *msg,
		    void *priv)
{
	struct ctx *c = priv;

	(void) m;

	if (msg->topic_len == strlen(c->topic_req) &&
	    !strncmp(msg->topic, c->topic_req, msg->topic_len))
		on_req(c->b, msg->payload, msg->payload_len);
	else if (msg->topic_len == strlen(c->topic_in) &&
		 !strncmp(msg->topic, c->topic_in, msg->topic_len))
		on_chunk(c->b, msg->payload, msg->payload_len);
}

int main(int argc, char **argv, char **envp)
{
	char t_req[128], t_rsp[128], t_in[128], t_out[128], clientid[64];
	struct bridge b = { 0 };
	struct smolmqtt m = { 0 };
	unsigned long long max_size;
	struct ctx c;
	const char *dir, *broker, *base;
	int ret, opt;

	(void)envp;

	b.fd = -1;
	b.max_size = DEFAULT_MAX_SIZE;

	/* Progress should show up in a log file or a pipe as it happens. */
	setvbuf(stdout, NULL, _IONBF, 0);

	while ((opt = getopt(argc, argv, "rs:")) != -1) {
		switch (opt) {
		case 'r':
			b.read_only = 1;
			break;
		case 's':
			if (!parse_size(optarg, &max_size) || !max_size ||
			    max_size > UINT32_MAX) {
				error("not a size: %s\n", optarg);
				return 1;
			}
			b.max_size = max_size;
			break;
		default:
			usage(USAGE, argv[0]);
			return 1;
		}
	}

	if (argc - optind != 3) {
		usage(USAGE, argv[0]);
		return 1;
	}
	dir = argv[optind];
	broker = argv[optind + 1];
	base = argv[optind + 2];

	snprintf(t_req, sizeof(t_req), "%s/req", base);
	snprintf(t_rsp, sizeof(t_rsp), "%s/rsp", base);
	snprintf(t_in, sizeof(t_in), "%s/in", base);
	snprintf(t_out, sizeof(t_out), "%s/out", base);
	snprintf(clientid, sizeof(clientid), "file2mqtt-%d", (int) getpid());

	b.m = &m;
	b.dir = dir;
	b.topic_rsp = t_rsp;
	b.topic_out = t_out;

	ret = smolmqtt_connect(&m, broker, 1883, clientid);
	if (ret) {
		error("connect failed: %d\n", ret);
		return 1;
	}

	ret = smolmqtt_subscribe(&m, t_req, 0);
	if (!ret)
		ret = smolmqtt_subscribe(&m, t_in, 0);
	if (ret) {
		error("subscribe failed: %d\n", ret);
		return 1;
	}

	printf("serving %s <-> %s (req=%s rsp=%s in=%s out=%s, %s, max %u bytes)\n",
	       dir, broker, t_req, t_rsp, t_in, t_out,
	       b.read_only ? "read-only" : "read/write", b.max_size);

	c.b = &b;
	c.topic_req = t_req;
	c.topic_in = t_in;

	for (;;) {
		if (smolmqtt_poll(&m, on_mqtt, &c))
			break;
	}

	smolmqtt_disconnect(&m);
	push_reset(&b);

	return 1;
}
