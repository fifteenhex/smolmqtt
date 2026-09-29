/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Bridge a directory to MQTT: files can be pulled from it and pushed into it
 * over a single outbound MQTT connection.
 *
 *	file2mqtt [-r] [-s maxsize] <directory> <broker_ip> <topic>
 *
 * -r makes it read-only (pulls work, pushes are refused) and -s caps the size
 * of a push in bytes (default 32 MiB).  Files are addressed by bare name; any
 * name containing a slash, or starting with a dot, is refused, so nothing
 * outside <directory> can be reached.
 *
 * The protocol lives in filemqtt.h.  Nothing is buffered whole: chunks are
 * written straight to <name>.part at their offset, and the finished file is
 * only renamed into place once its size and CRC match what the sender
 * promised.
 */
#include "filemqtt.h"
#include "smolmqtt.h"

#ifndef NOLIBC
/* -std=c99 hides readdir_r() and friends behind a feature test macro. */
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#endif

#define DEFAULT_MAX_SIZE	(32u * 1024 * 1024)

/* nolibc has no rename(); renameat2 is the syscall behind it everywhere we
 * care about.  The libc build just uses rename().
 */
#ifdef NOLIBC
#ifndef AT_FDCWD
#define AT_FDCWD	-100
#endif
static int file_rename(const char *from, const char *to)
{
	return __nolibc_syscall5(__NR_renameat2, AT_FDCWD, from, AT_FDCWD, to,
				 0) < 0 ? -1 : 0;
}
#else
#define file_rename rename
#endif

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
	char part[FILEMQTT_NAME_MAX + 8];
	uint32_t size;
	uint32_t crc;
	uint32_t chunks;
	uint8_t *map;
};

static void say(struct bridge *b, const char *s)
{
	uint32_t n = 0;

	while (s[n])
		n++;
	smolmqtt_publish(b->m, b->topic_rsp, s, n, 0, false);
}

/* Small formatter: "%s" and "%u" only, which is all the replies need. */
static void sayf(struct bridge *b, const char *fmt, ...)
{
	char out[FILEMQTT_LINE];
	__builtin_va_list ap;
	uint32_t o = 0;

	__builtin_va_start(ap, fmt);
	while (*fmt && o < sizeof(out) - 1) {
		if (*fmt != '%') {
			out[o++] = *fmt++;
			continue;
		}
		fmt++;
		if (*fmt == 's') {
			const char *s = __builtin_va_arg(ap, const char *);

			while (*s && o < sizeof(out) - 1)
				out[o++] = *s++;
		} else if (*fmt == 'u') {
			uint32_t v = __builtin_va_arg(ap, uint32_t);
			char tmp[12];
			int t = 0;

			do {
				tmp[t++] = (char) ('0' + v % 10);
				v /= 10;
			} while (v);
			while (t && o < sizeof(out) - 1)
				out[o++] = tmp[--t];
		} else if (*fmt == 'x') {
			uint32_t v = __builtin_va_arg(ap, uint32_t);
			int t;

			for (t = 7; t >= 0 && o < sizeof(out) - 1; t--)
				out[o++] = "0123456789abcdef"[(v >> (t * 4)) & 15];
		} else {
			out[o++] = *fmt;
		}
		fmt++;
	}
	__builtin_va_end(ap);

	smolmqtt_publish(b->m, b->topic_rsp, out, o, 0, false);
}

static int name_ok(const char *n)
{
	int i;

	if (!n[0] || n[0] == '.')
		return 0;
	for (i = 0; n[i]; i++) {
		if (i >= FILEMQTT_NAME_MAX - 1)
			return 0;
		if (n[i] == '/')
			return 0;
	}

	return 1;
}

static void path_of(char *out, uint32_t out_sz, const char *dir,
		    const char *name, const char *suffix)
{
	uint32_t o = 0;

	while (*dir && o < out_sz - 1)
		out[o++] = *dir++;
	if (o && out[o - 1] != '/' && o < out_sz - 1)
		out[o++] = '/';
	while (*name && o < out_sz - 1)
		out[o++] = *name++;
	while (suffix && *suffix && o < out_sz - 1)
		out[o++] = *suffix++;
	out[o] = 0;
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

static void cmd_list(struct bridge *b)
{
	uint32_t count = 0;
	struct dirent *e;
	DIR *d;

	d = opendir(b->dir);
	if (!d) {
		say(b, "err cannot open directory");
		return;
	}

	/* nolibc only has the reentrant readdir_r(); modern glibc only has
	 * readdir().  Same loop either way.
	 */
	for (;;) {
		char path[FILEMQTT_LINE];
		struct stat st;
#ifdef NOLIBC
		struct dirent ent;

		if (readdir_r(d, &ent, &e) || !e)
			break;
		e = &ent;
#else
		e = readdir(d);
		if (!e)
			break;
#endif
		if (e->d_name[0] == '.')
			continue;
		path_of(path, sizeof(path), b->dir, e->d_name, NULL);
		if (stat(path, &st) || !S_ISREG(st.st_mode))
			continue;
		sayf(b, "file %s %u", e->d_name, (uint32_t) st.st_size);
		count++;
	}

	closedir(d);
	sayf(b, "ok list %u", count);
}

static void cmd_get(struct bridge *b, const char *name)
{
	char path[FILEMQTT_LINE];
	uint8_t chunk[FILEMQTT_HDR + FILEMQTT_CHUNK];
	uint32_t crc = 0, total = 0, idx = 0;
	struct stat st;
	int fd;

	if (!name_ok(name)) {
		say(b, "err bad name");
		return;
	}
	path_of(path, sizeof(path), b->dir, name, NULL);
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
	sayf(b, "ok get %s %u %x %u", name, total, crc, idx);
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

	{
		int i = 0;

		while (name[i]) {
			b->name[i] = name[i];
			i++;
		}
		b->name[i] = 0;
	}
	path_of(b->part, sizeof(b->part), b->dir, b->name, ".part");

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
	uint32_t i, listed = 0, o = 0;

	if (b->fd < 0) {
		say(b, "err no transfer in progress");
		return;
	}

	for (i = 0; i < 8 && i < sizeof(out); i++)
		out[o++] = "missing "[i];

	for (i = 0; i < b->chunks && listed < FILEMQTT_MAX_MISSING; i++) {
		char tmp[12];
		uint32_t v = i;
		int t = 0;

		if (filemqtt_bit_get(b->map, i))
			continue;
		do {
			tmp[t++] = (char) ('0' + v % 10);
			v /= 10;
		} while (v);
		while (t && o < sizeof(out) - 2)
			out[o++] = tmp[--t];
		out[o++] = ' ';
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
		sayf(b, "err mismatch: got %u bytes crc %x, wanted %u %x",
		     total, crc, b->size, b->crc);
		printf("put %s FAILED: got %u/%x wanted %u/%x\n", b->name,
		       total, crc, b->size, b->crc);
		push_reset(b);
		return;
	}

	close(b->fd);
	b->fd = -1;

	path_of(path, sizeof(path), b->dir, b->name, NULL);
	if (file_rename(b->part, path)) {
		say(b, "err rename failed");
		push_reset(b);
		return;
	}

	printf("put %s: complete, %u bytes, crc %08x\n", b->name, total, crc);
	sayf(b, "ok put %s %u %x", b->name, total, crc);
	push_done(b, 1);
}

/* ---- dispatch ----------------------------------------------------------- */

static void on_req(struct bridge *b, const uint8_t *payload, uint32_t len)
{
	char line[FILEMQTT_LINE];
	char word[FILEMQTT_NAME_MAX];
	const char *p = line;
	uint32_t i, n = 0;

	for (i = 0; i < len && n < sizeof(line) - 1; i++) {
		if (payload[i] == '\r' || payload[i] == '\n')
			break;
		line[n++] = (char) payload[i];
	}
	line[n] = 0;

	filemqtt_word(&p, word, sizeof(word));

	if (!strcmp(word, "list")) {
		cmd_list(b);
	} else if (!strcmp(word, "get")) {
		filemqtt_word(&p, word, sizeof(word));
		cmd_get(b, word);
	} else if (!strcmp(word, "put")) {
		char name[FILEMQTT_NAME_MAX];
		uint32_t size, crc;

		filemqtt_word(&p, name, sizeof(name));
		size = filemqtt_num(&p, 0);
		crc = filemqtt_num(&p, 1);
		cmd_put(b, name, size, crc);
	} else if (!strcmp(word, "missing")) {
		cmd_missing(b);
	} else if (!strcmp(word, "end")) {
		cmd_end(b);
	} else if (!strcmp(word, "abort")) {
		push_reset(b);
		say(b, "ok abort");
	} else if (word[0]) {
		sayf(b, "err unknown command %s", word);
	}
}

static void on_chunk(struct bridge *b, const uint8_t *payload, uint32_t len)
{
	uint32_t idx, n;
	off_t off;

	if (b->fd < 0 || len < FILEMQTT_HDR)
		return;

	idx = filemqtt_be32(payload);
	n = len - FILEMQTT_HDR;
	if (idx >= b->chunks || n > FILEMQTT_CHUNK)
		return;

	off = (off_t) idx * FILEMQTT_CHUNK;
	if (lseek(b->fd, off, SEEK_SET) < 0)
		return;
	if (write(b->fd, payload + FILEMQTT_HDR, n) != (ssize_t) n)
		return;

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

static void usage(const char *argv0)
{
	printf("usage: %s [-r] [-s maxsize] <directory> <broker_ip> <topic>\n",
	       argv0);
}

int main(int argc, char **argv)
{
	char t_req[128], t_rsp[128], t_in[128], t_out[128], clientid[64];
	struct bridge b = { 0 };
	struct smolmqtt m = { 0 };
	struct ctx c;
	const char *dir, *broker, *base;
	int ret, opt;

	b.fd = -1;
	b.max_size = DEFAULT_MAX_SIZE;

#ifndef NOLIBC
	/* Progress should show up in a log file or a pipe as it happens. */
	setvbuf(stdout, NULL, _IONBF, 0);
#endif

	while ((opt = getopt(argc, argv, "rs:")) != -1) {
		switch (opt) {
		case 'r':
			b.read_only = 1;
			break;
		case 's':
			b.max_size = (uint32_t) atoi(optarg);
			break;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	if (argc - optind != 3) {
		usage(argv[0]);
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
		printf("connect failed: %d\n", ret);
		return 1;
	}

	ret = smolmqtt_subscribe(&m, t_req, 0);
	if (!ret)
		ret = smolmqtt_subscribe(&m, t_in, 0);
	if (ret) {
		printf("subscribe failed: %d\n", ret);
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
