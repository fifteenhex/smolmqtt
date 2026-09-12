/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Bridge a serial port to MQTT: bytes read from the port are published to
 * <topic>/rx, and messages on <topic>/tx are written to the port.
 *
 *   serial2mqtt [-b baud] [-c 8N1] [-m ascii|data] <serial> <broker_ip> <topic>
 *
 * ascii mode passes the bytes through as-is; data mode base64-encodes each
 * way, so arbitrary binary survives.
 */
#include "smolmqtt.h"

#ifndef NOLIBC
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#endif

#include <asm/termbits.h>

#define BUF_SZ	512

enum mode { MODE_ASCII, MODE_DATA };

/* --- base64 --- */

static const char b64_alpha[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static uint32_t b64_encode(const uint8_t *in, uint32_t len, char *out)
{
	uint32_t o = 0, i = 0;

	while (i + 3 <= len) {
		uint32_t v = (in[i] << 16) | (in[i + 1] << 8) | in[i + 2];

		out[o++] = b64_alpha[(v >> 18) & 63];
		out[o++] = b64_alpha[(v >> 12) & 63];
		out[o++] = b64_alpha[(v >> 6) & 63];
		out[o++] = b64_alpha[v & 63];
		i += 3;
	}

	if (len - i == 1) {
		uint32_t v = in[i] << 16;

		out[o++] = b64_alpha[(v >> 18) & 63];
		out[o++] = b64_alpha[(v >> 12) & 63];
		out[o++] = '=';
		out[o++] = '=';
	} else if (len - i == 2) {
		uint32_t v = (in[i] << 16) | (in[i + 1] << 8);

		out[o++] = b64_alpha[(v >> 18) & 63];
		out[o++] = b64_alpha[(v >> 12) & 63];
		out[o++] = b64_alpha[(v >> 6) & 63];
		out[o++] = '=';
	}

	return o;
}

static int b64_val(char c)
{
	if (c >= 'A' && c <= 'Z')
		return c - 'A';
	if (c >= 'a' && c <= 'z')
		return c - 'a' + 26;
	if (c >= '0' && c <= '9')
		return c - '0' + 52;
	if (c == '+')
		return 62;
	if (c == '/')
		return 63;
	return -1;
}

static uint32_t b64_decode(const uint8_t *in, uint32_t len, uint8_t *out)
{
	uint32_t o = 0, i;
	int acc = 0, bits = 0;

	for (i = 0; i < len; i++) {
		int v = b64_val((char) in[i]);

		if (v < 0)		/* skip '=' and any whitespace */
			continue;
		acc = (acc << 6) | v;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			out[o++] = (uint8_t) ((acc >> bits) & 0xff);
		}
	}

	return o;
}

/* --- serial --- */

static int serial_open(const char *dev, int baud, int databits, char parity, int stopbits)
{
	struct termios2 tio;
	int fd;

	fd = open(dev, O_RDWR | O_NOCTTY);
	if (fd < 0)
		return -1;

	if (ioctl(fd, TCGETS2, &tio)) {
		close(fd);
		return -1;
	}

	/* raw: no input/output/line processing */
	tio.c_iflag = 0;
	tio.c_oflag = 0;
	tio.c_lflag = 0;
	tio.c_cflag = CREAD | CLOCAL;

	tio.c_cflag |= databits == 5 ? CS5 : databits == 6 ? CS6 :
		       databits == 7 ? CS7 : CS8;
	if (parity == 'E')
		tio.c_cflag |= PARENB;
	else if (parity == 'O')
		tio.c_cflag |= PARENB | PARODD;
	if (stopbits == 2)
		tio.c_cflag |= CSTOPB;

	/* BOTHER lets us set an arbitrary baud in c_ispeed/c_ospeed */
	tio.c_cflag |= BOTHER;
	tio.c_ispeed = baud;
	tio.c_ospeed = baud;

	tio.c_cc[VMIN] = 1;
	tio.c_cc[VTIME] = 0;

	if (ioctl(fd, TCSETS2, &tio)) {
		close(fd);
		return -1;
	}

	return fd;
}

static int write_all(int fd, const uint8_t *buf, uint32_t len)
{
	uint32_t done = 0;

	while (done < len) {
		ssize_t n = write(fd, buf + done, len - done);

		if (n > 0)
			done += (uint32_t) n;
		else if (n < 0 && errno == EINTR)
			continue;
		else
			return -1;
	}

	return 0;
}

/* --- bridge --- */

struct bridge {
	int serial_fd;
	enum mode mode;
};

/* MQTT -> serial */
static void on_mqtt(struct smolmqtt *m, const struct smolmqtt_message *msg, void *priv)
{
	struct bridge *b = priv;

	(void) m;

	if (b->mode == MODE_DATA) {
		uint8_t out[BUF_SZ];
		uint32_t n = b64_decode(msg->payload, msg->payload_len, out);

		write_all(b->serial_fd, out, n);
	} else {
		write_all(b->serial_fd, msg->payload, msg->payload_len);
	}
}

static void usage(const char *argv0)
{
	printf("usage: %s [-b baud] [-c 8N1] [-m ascii|data] <serial> <broker_ip> <topic>\n",
	       argv0);
}

int main(int argc, char **argv)
{
	struct bridge b = { .mode = MODE_ASCII };
	struct smolmqtt m = { 0 };
	int baud = 115200, databits = 8, stopbits = 1;
	char parity = 'N';
	const char *serial_dev, *broker, *base_topic;
	char topic_rx[128], topic_tx[128], clientid[64];
	struct pollfd fds[2];
	int ret, opt;

	while ((opt = getopt(argc, argv, "b:c:m:")) != -1) {
		switch (opt) {
		case 'b':
			baud = atoi(optarg);
			break;
		case 'c':
			if (optarg[0] && optarg[1] && optarg[2]) {
				databits = optarg[0] - '0';
				parity = optarg[1];
				stopbits = optarg[2] - '0';
			}
			break;
		case 'm':
			b.mode = optarg[0] == 'd' ? MODE_DATA : MODE_ASCII;
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
	serial_dev = argv[optind];
	broker = argv[optind + 1];
	base_topic = argv[optind + 2];

	snprintf(topic_rx, sizeof(topic_rx), "%s/rx", base_topic);
	snprintf(topic_tx, sizeof(topic_tx), "%s/tx", base_topic);
	snprintf(clientid, sizeof(clientid), "serial2mqtt-%d", (int) getpid());

	b.serial_fd = serial_open(serial_dev, baud, databits, parity, stopbits);
	if (b.serial_fd < 0) {
		printf("failed to open %s\n", serial_dev);
		return 1;
	}

	ret = smolmqtt_connect(&m, broker, 1883, clientid);
	if (ret) {
		printf("connect failed: %d\n", ret);
		return 1;
	}

	ret = smolmqtt_subscribe(&m, topic_tx, 0);
	if (ret) {
		printf("subscribe failed: %d\n", ret);
		return 1;
	}

	printf("bridging %s <-> %s (rx=%s tx=%s, %s mode)\n",
	       serial_dev, broker, topic_rx, topic_tx,
	       b.mode == MODE_DATA ? "data" : "ascii");

	fds[0].fd = b.serial_fd;
	fds[0].events = POLLIN;
	fds[1].fd = m.fd;
	fds[1].events = POLLIN;

	for (;;) {
		fds[0].revents = fds[1].revents = 0;

		if (poll(fds, 2, -1) < 0) {
			if (errno == EINTR)
				continue;
			break;
		}

		/* serial -> MQTT */
		if (fds[0].revents & POLLIN) {
			uint8_t buf[BUF_SZ];
			ssize_t n = read(b.serial_fd, buf, sizeof(buf));

			if (n > 0) {
				if (b.mode == MODE_DATA) {
					char enc[BUF_SZ * 4 / 3 + 4];
					uint32_t el = b64_encode(buf, (uint32_t) n, enc);

					smolmqtt_publish(&m, topic_rx, enc, el, 0, false);
				} else {
					smolmqtt_publish(&m, topic_rx, buf, (uint32_t) n, 0, false);
				}
			}
		}

		/* MQTT -> serial */
		if (fds[1].revents & POLLIN) {
			if (smolmqtt_poll(&m, on_mqtt, &b))
				break;
		}
	}

	smolmqtt_disconnect(&m);
	close(b.serial_fd);
	return 1;
}
