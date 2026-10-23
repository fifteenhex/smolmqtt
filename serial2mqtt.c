/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Bridge a serial port to MQTT: bytes read from the port are published to
 * <topic>/rx, and messages on <topic>/tx are written to the port.
 *
 *	serial2mqtt [-b baud] [-c 8N1] [-f none|dtr] [-m ascii|data]
 *		<serial> <broker_ip> <topic>
 *
 * ascii mode passes the bytes through as-is; data mode base64-encodes each
 * way, so arbitrary binary survives.
 */
#include "smolmqtt.h"
#include "base64.h"

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

#include "config.h"
#include "inc/printing.h"

#define USAGE	"usage: %s [-b baud] [-c 8N1] [-f none|dtr] [-m ascii|data] " \
		"<serial> <broker_ip> <topic>\n"

#define BUF_SZ	512

/* Linux termios has no DTR/DSR flow control, so it is done by hand below.
 * <sys/ioctl.h> defines these, but the nolibc build does not get it.
 */
#ifndef TIOCM_DTR
#define TIOCM_DTR	0x002
#endif
#ifndef TIOCM_RTS
#define TIOCM_RTS	0x004
#endif
#ifndef TIOCM_DSR
#define TIOCM_DSR	0x100
#endif

/* Longest to wait on a write before giving up on it. */
#define DSR_WAIT_MS	1000

/* Bytes between DSR checks. Devices tolerate a few more after holdoff. */
#define DSR_CHUNK	8

enum mode { MODE_ASCII, MODE_DATA };
enum flow { FLOW_NONE, FLOW_DTR };

static int serial_open(const char *dev, int baud, int databits, char parity,
		       int stopbits, enum flow flow)
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

	if (flow == FLOW_DTR) {
		int bits = TIOCM_DTR | TIOCM_RTS;

		/* Not fatal: the kernel raises these on open anyway, and a
		 * pty has no modem lines to raise.
		 */
		if (ioctl(fd, TIOCMBIS, &bits))
			error("cannot assert DTR/RTS on %s\n", dev);
	}

	return fd;
}

/* With a null modem the far end's DTR arrives on our DSR, and it drops the
 * line to hold us off.
 */
static int dsr_ready(int fd)
{
	int status;

	if (ioctl(fd, TIOCMGET, &status))
		return 1;	/* cannot tell, so do not wedge the bridge */

	return !!(status & TIOCM_DSR);
}

static int wait_for_dsr(int fd)
{
	int waited;

	for (waited = 0; waited < DSR_WAIT_MS; waited++) {
		if (dsr_ready(fd))
			return 0;
		poll(NULL, 0, 1);
	}

	return -1;
}

static int write_all(int fd, const uint8_t *buf, uint32_t len, enum flow flow)
{
	uint32_t done = 0;

	while (done < len) {
		uint32_t want = len - done;
		ssize_t n;

		if (flow == FLOW_DTR) {
			if (wait_for_dsr(fd))
				return -1;
			if (want > DSR_CHUNK)
				want = DSR_CHUNK;
		}

		n = write(fd, buf + done, want);

		if (n > 0)
			done += (uint32_t) n;
		else if (n < 0 && errno == EINTR)
			continue;
		else
			return -1;
	}

	return 0;
}

struct bridge {
	int serial_fd;
	enum mode mode;
	enum flow flow;
};

/* MQTT -> serial */
static void on_mqtt(struct smolmqtt *m, const struct smolmqtt_message *msg,
		    void *priv)
{
	struct bridge *b = priv;

	(void) m;

	if (b->mode == MODE_DATA) {
		uint8_t out[BUF_SZ];
		uint32_t n = b64_decode(msg->payload, msg->payload_len, out);

		if (write_all(b->serial_fd, out, n, b->flow))
			error("write failed\n");
	} else {
		if (write_all(b->serial_fd, msg->payload, msg->payload_len,
			      b->flow))
			error("write failed\n");
	}
}

int main(int argc, char **argv, char **envp)
{
	struct bridge b = { .mode = MODE_ASCII };
	struct smolmqtt m = { 0 };
	int baud = 115200, databits = 8, stopbits = 1;
	char parity = 'N';
	const char *serial_dev, *broker, *base_topic;
	char topic_rx[128], topic_tx[128], clientid[64];
	struct pollfd fds[2];
	int ret, opt;

	(void)envp;

	while ((opt = getopt(argc, argv, "b:c:f:m:")) != -1) {
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
		case 'f':
			b.flow = optarg[0] == 'd' ? FLOW_DTR : FLOW_NONE;
			break;
		case 'm':
			b.mode = optarg[0] == 'd' ? MODE_DATA : MODE_ASCII;
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
	serial_dev = argv[optind];
	broker = argv[optind + 1];
	base_topic = argv[optind + 2];

	snprintf(topic_rx, sizeof(topic_rx), "%s/rx", base_topic);
	snprintf(topic_tx, sizeof(topic_tx), "%s/tx", base_topic);
	snprintf(clientid, sizeof(clientid), "serial2mqtt-%d", (int) getpid());

	b.serial_fd = serial_open(serial_dev, baud, databits, parity, stopbits,
				  b.flow);
	if (b.serial_fd < 0) {
		error("failed to open %s\n", serial_dev);
		return 1;
	}

	ret = smolmqtt_connect(&m, broker, 1883, clientid);
	if (ret) {
		error("connect failed: %d\n", ret);
		return 1;
	}

	ret = smolmqtt_subscribe(&m, topic_tx, 0);
	if (ret) {
		error("subscribe failed: %d\n", ret);
		return 1;
	}

	printf("bridging %s <-> %s (rx=%s tx=%s, %s mode, %s flow control)\n",
	       serial_dev, broker, topic_rx, topic_tx,
	       b.mode == MODE_DATA ? "data" : "ascii",
	       b.flow == FLOW_DTR ? "dtr/dsr" : "no");

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
