/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Subscribe to a topic and print each message as it arrives.
 *
 *	./smolmqtt_sub <broker_ip> <topic>
 */
#include "smolmqtt.h"

#ifndef NOLIBC
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#endif

static void on_msg(struct smolmqtt *m, const struct smolmqtt_message *msg,
		   void *priv)
{
	char topic[256], payload[256];
	unsigned int tl = msg->topic_len;
	unsigned int pl = msg->payload_len;

	(void) m;
	(void) priv;

	if (tl > sizeof(topic) - 1)
		tl = sizeof(topic) - 1;
	if (pl > sizeof(payload) - 1)
		pl = sizeof(payload) - 1;

	memcpy(topic, msg->topic, tl);
	topic[tl] = 0;
	memcpy(payload, msg->payload, pl);
	payload[pl] = 0;

	printf("%s: %s\n", topic, payload);
}

int main(int argc, char **argv)
{
	struct smolmqtt m = { 0 };
	char clientid[64];
	int ret;

	if (argc != 3) {
		printf("usage: %s <broker_ip> <topic>\n", argv[0]);
		return 1;
	}

	snprintf(clientid, sizeof(clientid), "smolmqtt-sub-%d", (int) getpid());

	ret = smolmqtt_connect(&m, argv[1], 1883, clientid);
	if (ret) {
		printf("connect failed: %d\n", ret);
		return 1;
	}

	ret = smolmqtt_subscribe(&m, argv[2], 0);
	if (ret) {
		printf("subscribe failed: %d\n", ret);
		smolmqtt_disconnect(&m);
		return 1;
	}
	printf("subscribed to %s, waiting for messages...\n", argv[2]);

	for (;;) {
		ret = smolmqtt_poll(&m, on_msg, NULL);
		if (ret) {
			printf("poll failed: %d\n", ret);
			break;
		}
	}

	smolmqtt_disconnect(&m);
	return 1;
}
