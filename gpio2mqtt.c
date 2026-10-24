/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Bridge gpio lines to MQTT: every edge on an input is published to
 * <topic>/in/<offset> as 0 or 1, and a 0 or 1 sent to <topic>/out/<offset>
 * drives an output.
 *
 *	gpio2mqtt [-l] [-i <offset>]... [-o <offset>[=<value>]]...
 *		<chip> <broker_ip> <topic>
 *
 * The inputs are published once at start too, retained, so a late
 * subscriber sees the state and not just the next change. Outputs start at
 * <value>, 0 if there is none. -l makes every line active low. <chip> is
 * gpiochip<n>, <n> or a path.
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
#include <dirent.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#endif

#include "config.h"
#include "inc/printing.h"
#include "inc/numbers.h"
#include "inc/strings.h"
#include "inc/gpio.h"

#define USAGE	"usage: %s [-l] [-i <offset>]... [-o <offset>[=<value>]]... " \
		"<chip> <broker_ip> <topic>\n"

#define CONSUMER	"gpio2mqtt"
#define TOPIC_SZ	128

/* One request, inputs or outputs */
struct lines {
	unsigned int offsets[GPIO_V2_LINES_MAX];
	unsigned int num;
	uint64_t values;
	int fd;
};

struct bridge {
	struct smolmqtt *m;
	const char *topic;
	struct lines in;
	struct lines out;
};

/* <offset> for an input, <offset>[=<value>] for an output */
static bool add_line(struct lines *l, char *arg, bool output)
{
	char *eq = output ? strchr(arg, '=') : NULL;
	unsigned long v = 0;

	if (l->num == GPIO_V2_LINES_MAX) {
		error("Too many lines\n");
		return false;
	}

	if (eq) {
		if (!parse_ul_range(eq + 1, 10, 0, 1, &v)) {
			error("Not an <offset>=<value>: %s\n", arg);
			return false;
		}
		*eq = '\0';
	}

	if (!gpio_parse_offset(arg, &l->offsets[l->num]))
		return false;

	l->values |= (uint64_t) v << l->num;
	l->num++;

	return true;
}

static int publish_value(struct bridge *b, unsigned int offset,
			 unsigned int value)
{
	char payload = '0' + value;
	char topic[TOPIC_SZ];
	int ret;

	if (!str_format(topic, "%s/in/%u", b->topic, offset)) {
		error("Topic is too long\n");
		return -1;
	}

	ret = smolmqtt_publish(b->m, topic, &payload, 1, 0, true);
	if (ret)
		error("Publish failed: %d\n", ret);

	return ret;
}

/* The state at start, so there is something to see before an edge */
static int publish_inputs(struct bridge *b)
{
	uint64_t values;
	unsigned int i;

	if (gpio_get_values(b->in.fd, gpio_lines_mask(b->in.num), &values))
		return -1;

	for (i = 0; i < b->in.num; i++) {
		if (publish_value(b, b->in.offsets[i], (values >> i) & 1))
			return -1;
	}

	return 0;
}

static int publish_event(struct bridge *b)
{
	struct gpio_v2_line_event event;

	if (gpio_read_event(b->in.fd, &event))
		return -1;

	return publish_value(b, event.offset,
			     event.id == GPIO_V2_LINE_EVENT_RISING_EDGE);
}

/* MQTT -> output, the topic is <topic>/out/<offset> */
static void on_mqtt(struct smolmqtt *m, const struct smolmqtt_message *msg,
		    void *priv)
{
	struct bridge *b = priv;
	char topic[TOPIC_SZ];
	unsigned int offset;
	unsigned int i;
	size_t prefix;

	(void) m;

	prefix = strlen(b->topic) + STRLEN("/out/");
	if (msg->topic_len >= sizeof(topic) || msg->topic_len <= prefix)
		return;
	memcpy(topic, msg->topic, msg->topic_len);
	topic[msg->topic_len] = '\0';

	if (!gpio_parse_offset(topic + prefix, &offset))
		return;

	for (i = 0; i < b->out.num; i++) {
		if (b->out.offsets[i] == offset)
			break;
	}

	if (i == b->out.num) {
		error("%u isn't an output\n", offset);
		return;
	}

	if (msg->payload_len != 1 ||
	    (msg->payload[0] != '0' && msg->payload[0] != '1')) {
		error("Not a value for %s: %.*s\n", topic,
		      (int) msg->payload_len, (const char *) msg->payload);
		return;
	}

	gpio_set_values(b->out.fd, 1ULL << i,
			(uint64_t) (msg->payload[0] - '0') << i);
}

int main(int argc, char **argv, char **envp)
{
	struct bridge b = { .in.fd = -1, .out.fd = -1 };
	struct smolmqtt m = { 0 };
	uint64_t flags = 0;
	char topic_out[TOPIC_SZ];
	char clientid[64];
	struct pollfd fds[2];
	const char *broker;
	int chip;
	int ret;
	int opt;

	(void) envp;

	while ((opt = getopt(argc, argv, "li:o:")) != -1) {
		switch (opt) {
		case 'l':
			flags |= GPIO_V2_LINE_FLAG_ACTIVE_LOW;
			break;
		case 'i':
			if (!add_line(&b.in, optarg, false))
				return 1;
			break;
		case 'o':
			if (!add_line(&b.out, optarg, true))
				return 1;
			break;
		default:
			usage(USAGE, argv[0]);
			return 1;
		}
	}

	if (argc - optind != 3 || (!b.in.num && !b.out.num)) {
		usage(USAGE, argv[0]);
		return 1;
	}
	broker = argv[optind + 1];
	b.topic = argv[optind + 2];
	b.m = &m;

	if (!str_format(topic_out, "%s/out/+", b.topic)) {
		error("Topic is too long\n");
		return 1;
	}
	snprintf(clientid, sizeof(clientid), CONSUMER "-%d", (int) getpid());

	chip = gpio_open_chip(argv[optind], O_RDONLY);
	if (chip < 0)
		return 1;

	if (b.in.num) {
		b.in.fd = gpio_request_lines(chip, b.in.offsets, b.in.num,
					     flags | GPIO_V2_LINE_FLAG_INPUT |
					     GPIO_V2_LINE_FLAG_EDGE_RISING |
					     GPIO_V2_LINE_FLAG_EDGE_FALLING,
					     0, CONSUMER);
		if (b.in.fd < 0)
			return 1;
	}

	if (b.out.num) {
		b.out.fd = gpio_request_lines(chip, b.out.offsets, b.out.num,
					      flags | GPIO_V2_LINE_FLAG_OUTPUT,
					      b.out.values, CONSUMER);
		if (b.out.fd < 0)
			return 1;
	}

	/* The requests outlive the chip */
	close(chip);

	ret = smolmqtt_connect(&m, broker, 1883, clientid);
	if (ret) {
		error("connect failed: %d\n", ret);
		return 1;
	}

	if (b.out.num) {
		ret = smolmqtt_subscribe(&m, topic_out, 0);
		if (ret) {
			error("subscribe failed: %d\n", ret);
			return 1;
		}
	}

	if (b.in.num && publish_inputs(&b))
		return 1;

	printf("bridging %s <-> %s (%u inputs on %s/in, %u outputs on %s)\n",
	       argv[optind], broker, b.in.num, b.topic, b.out.num, topic_out);

	/* poll() skips a negative fd, so no inputs is fine */
	fds[0].fd = b.in.fd;
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

		if (fds[0].revents & POLLIN && publish_event(&b))
			break;

		if (fds[1].revents & POLLIN && smolmqtt_poll(&m, on_mqtt, &b))
			break;
	}

	smolmqtt_disconnect(&m);

	return 1;
}
