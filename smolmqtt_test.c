/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Roundtrip self-test: connect, subscribe to a unique topic, publish to it,
 * and confirm the broker delivers it back. Exits 0 on success.
 *
 *	./smolmqtt_test [broker_ip]	(default 192.168.3.2)
 */
#include "smolmqtt.h"

#ifndef NOLIBC
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#endif

#define BROKER_DEFAULT	"192.168.3.2"
#define BROKER_PORT	1883
#define MESSAGE		"hello from smolmqtt"

static bool got_it;

static void on_msg(struct smolmqtt *m, const struct smolmqtt_message *msg,
		   void *priv)
{
	char topic[128], payload[128];
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

	printf("RX topic='%s' payload='%s' qos=%u\n", topic, payload, msg->qos);

	if (msg->payload_len == strlen(MESSAGE) &&
	    memcmp(msg->payload, MESSAGE, msg->payload_len) == 0)
		got_it = true;
}

int main(int argc, char **argv)
{
	const char *broker = argc > 1 ? argv[1] : BROKER_DEFAULT;
	struct smolmqtt m = { 0 };
	char topic[64], clientid[64];
	int pid = (int) getpid();
	int ret;

	snprintf(clientid, sizeof(clientid), "smolmqtt-test-%d", pid);
	snprintf(topic, sizeof(topic), "smolmqtt/selftest/%d", pid);

	printf("connecting to %s:%d\n", broker, BROKER_PORT);
	ret = smolmqtt_connect(&m, broker, BROKER_PORT, clientid);
	if (ret) {
		printf("connect failed: %d\n", ret);
		return 1;
	}

	/*
	 * The roundtrip has to be QoS 0: the subscription is on the topic
	 * being published to, so a QoS 1 publish would put the PUBACK and the
	 * delivered PUBLISH on the one socket in either order. QoS 1 gets its
	 * own check below, on a topic with no subscriber.
	 */
	ret = smolmqtt_subscribe(&m, topic, 0);
	if (ret) {
		printf("subscribe failed: %d\n", ret);
		return 1;
	}
	printf("subscribed to %s\n", topic);

	ret = smolmqtt_publish(&m, topic, MESSAGE, strlen(MESSAGE), 0, false);
	if (ret) {
		printf("publish failed: %d\n", ret);
		return 1;
	}
	printf("published, waiting for it to come back...\n");

	ret = smolmqtt_poll(&m, on_msg, NULL);
	if (ret) {
		printf("poll failed: %d\n", ret);
		return 1;
	}

	if (!got_it) {
		printf("ROUNDTRIP FAILED (message not received)\n");
		smolmqtt_disconnect(&m);
		return 1;
	}

	snprintf(topic, sizeof(topic), "smolmqtt/selftest/%d/qos1", pid);
	ret = smolmqtt_publish(&m, topic, "qos1", 4, 1, false);
	if (ret) {
		printf("qos1 publish/PUBACK failed: %d\n", ret);
		smolmqtt_disconnect(&m);
		return 1;
	}
	printf("qos1 PUBACK OK\n");

	smolmqtt_disconnect(&m);
	printf("ROUNDTRIP OK\n");
	return 0;
}
