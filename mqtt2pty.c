/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * The other end of serial2mqtt: a pseudo-terminal on this machine that is
 * the serial port serial2mqtt is sharing. Bytes written to the pty go out as
 * messages on <topic>/tx, and messages on <topic>/rx come back out of it.
 *
 *	mqtt2pty [-m ascii|data] [-l <link>] <broker_ip> <topic>
 *
 * The path of the pty (/dev/pts/N) is printed on stdout, and with -l a
 * symlink is kept at <link> so a tool can be pointed at a fixed name:
 *
 *	mqtt2pty -m data -l /tmp/clie 192.168.3.2 clielinux1 &
 *	smallrom -d /tmp/clie read 0x10000000 0x100
 *
 * The mode has to match the serial2mqtt at the far end: data mode is
 * base64 each way, which is what a binary protocol needs.
 *
 * The slave side is held open here as well, so a tool opening and closing
 * the pty does not hang up the master and the bridge survives any number
 * of them. The pty is put in raw mode; the tool's own termios settings (a
 * baud rate, say) are accepted and mean nothing, the real line's speed is
 * whatever serial2mqtt was started with.
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

/* A read from the pty; must base64 to under SMOLMQTT_RXBUF_SZ's worth */
#define BUF_SZ		512

enum mode { MODE_ASCII, MODE_DATA };

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

/* raw: nothing echoed, nothing translated, nothing waited for */
static int make_raw(int fd)
{
	struct termios2 tio;

	if (ioctl(fd, TCGETS2, &tio))
		return -1;

	tio.c_iflag = 0;
	tio.c_oflag = 0;
	tio.c_lflag = 0;
	tio.c_cflag = CREAD | CLOCAL | CS8;
	tio.c_cc[VMIN] = 1;
	tio.c_cc[VTIME] = 0;

	return ioctl(fd, TCSETS2, &tio);
}

/*
 * Open the master, unlock it, find out the slave's name and open that too
 * so the master never sees a hangup. Returns the master; the slave fd is
 * only kept open.
 */
static int pty_open(char *name, size_t name_sz, int *slave_fd)
{
	int master, unlock = 0, n;

	master = open("/dev/ptmx", O_RDWR | O_NOCTTY);
	if (master < 0)
		return -1;

	if (ioctl(master, TIOCSPTLCK, &unlock) || ioctl(master, TIOCGPTN, &n)) {
		close(master);
		return -1;
	}

	snprintf(name, name_sz, "/dev/pts/%d", n);

	*slave_fd = open(name, O_RDWR | O_NOCTTY);
	if (*slave_fd < 0) {
		close(master);
		return -1;
	}

	if (make_raw(*slave_fd)) {
		close(*slave_fd);
		close(master);
		return -1;
	}

	return master;
}

struct bridge {
	int master_fd;
	enum mode mode;
};

/* MQTT -> pty */
static void on_mqtt(struct smolmqtt *m, const struct smolmqtt_message *msg,
		    void *priv)
{
	struct bridge *b = priv;

	(void) m;

	if (b->mode == MODE_DATA) {
		uint8_t out[SMOLMQTT_RXBUF_SZ];
		uint32_t n = b64_decode(msg->payload, msg->payload_len, out);

		if (write_all(b->master_fd, out, n))
			printf("write failed\n");
	} else {
		if (write_all(b->master_fd, msg->payload, msg->payload_len))
			printf("write failed\n");
	}
}

static void usage(const char *argv0)
{
	printf("usage: %s [-m ascii|data] [-l <link>] <broker_ip> <topic>\n",
	       argv0);
}

int main(int argc, char **argv, char **envp)
{
	struct bridge b = { .mode = MODE_ASCII };
	struct smolmqtt m = { 0 };
	const char *broker, *base_topic, *link = NULL;
	char topic_rx[128], topic_tx[128], clientid[64], name[32];
	struct pollfd fds[2];
	int slave_fd, ret, opt;

	(void)envp;

	while ((opt = getopt(argc, argv, "m:l:")) != -1) {
		switch (opt) {
		case 'm':
			b.mode = optarg[0] == 'd' ? MODE_DATA : MODE_ASCII;
			break;
		case 'l':
			link = optarg;
			break;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	if (argc - optind != 2) {
		usage(argv[0]);
		return 1;
	}
	broker = argv[optind];
	base_topic = argv[optind + 1];

	snprintf(topic_rx, sizeof(topic_rx), "%s/rx", base_topic);
	snprintf(topic_tx, sizeof(topic_tx), "%s/tx", base_topic);
	snprintf(clientid, sizeof(clientid), "mqtt2pty-%d", (int) getpid());

	b.master_fd = pty_open(name, sizeof(name), &slave_fd);
	if (b.master_fd < 0) {
		printf("failed to open a pty\n");
		return 1;
	}

	if (link) {
		unlink(link);
		if (symlink(name, link)) {
			printf("failed to link %s to %s\n", link, name);
			return 1;
		}
	}

	ret = smolmqtt_connect(&m, broker, 1883, clientid);
	if (ret) {
		printf("connect failed: %d\n", ret);
		return 1;
	}

	ret = smolmqtt_subscribe(&m, topic_rx, 0);
	if (ret) {
		printf("subscribe failed: %d\n", ret);
		return 1;
	}

	/* The path first and on its own line, so a script can read it off */
	printf("%s\n", name);
	printf("bridging %s <-> %s (rx=%s tx=%s, %s mode)\n",
	       link ? link : name, broker, topic_rx, topic_tx,
	       b.mode == MODE_DATA ? "data" : "ascii");
	fflush(stdout);

	fds[0].fd = b.master_fd;
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

		/* pty -> MQTT */
		if (fds[0].revents & POLLIN) {
			uint8_t buf[BUF_SZ];
			ssize_t n = read(b.master_fd, buf, sizeof(buf));

			if (n > 0) {
				if (b.mode == MODE_DATA) {
					char enc[BUF_SZ * 4 / 3 + 4];
					uint32_t el = b64_encode(buf, (uint32_t) n, enc);

					smolmqtt_publish(&m, topic_tx, enc, el, 0, false);
				} else {
					smolmqtt_publish(&m, topic_tx, buf, (uint32_t) n, 0, false);
				}
			}
		}

		/* MQTT -> pty */
		if (fds[1].revents & POLLIN) {
			if (smolmqtt_poll(&m, on_mqtt, &b))
				break;
		}
	}

	smolmqtt_disconnect(&m);
	if (link)
		unlink(link);
	close(slave_fd);
	close(b.master_fd);
	return 1;
}
