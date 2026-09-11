/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Roundtrip self-test: connect, subscribe to a unique topic, publish to it,
 * and confirm the broker delivers it back. Exits 0 on success.
 *
 *   ./smolmqtt_test [broker_ip]      (default 192.168.3.2)
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

static void on_msg(struct smolmqtt *m, const struct smolmqtt_message *msg, void *priv)
{
	char topic[128], payload[128];
	unsigned int tl = msg->topic_len < sizeof(topic) - 1 ? msg->topic_len : sizeof(topic) - 1;
	unsigned int pl = msg->payload_len < sizeof(payload) - 1 ? msg->payload_len : sizeof(payload) - 1;

	(void) m; (void) priv;

	memcpy(topic, msg->topic, tl);   topic[tl] = 0;
	memcpy(payload, msg->payload, pl); payload[pl] = 0;

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
	 * QoS 0 for the roundtrip: we're subscribed to the same topic we
	 * publish to, and a QoS 1 publish would interleave its PUBACK with the
	 * delivered PUBLISH on the one socket. QoS 1's ack path is checked
	 * separately below on a topic nobody is subscribed to.
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

	/* Our own message should arrive as we're subscribed to the same topic */
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

	/* Exercise the QoS 1 path (waits for PUBACK) on an unsubscribed topic */
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
