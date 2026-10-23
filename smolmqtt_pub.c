/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Publish one message.
 *
 *	./smolmqtt_pub <broker_ip> <topic> <message>
 */
#include "smolmqtt.h"

#ifndef NOLIBC
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#endif

#include "config.h"
#include "inc/printing.h"

#define USAGE	"usage: %s <broker_ip> <topic> <message>\n"

int main(int argc, char **argv, char **envp)
{
	struct smolmqtt m = { 0 };
	char clientid[64];
	int ret;

	(void)envp;

	if (argc != 4) {
		usage(USAGE, argv[0]);
		return 1;
	}

	snprintf(clientid, sizeof(clientid), "smolmqtt-pub-%d", (int) getpid());

	ret = smolmqtt_connect(&m, argv[1], 1883, clientid);
	if (ret) {
		error("connect failed: %d\n", ret);
		return 1;
	}

	ret = smolmqtt_publish(&m, argv[2], argv[3], strlen(argv[3]), 1, false);
	if (ret) {
		error("publish failed: %d\n", ret);
		smolmqtt_disconnect(&m);
		return 1;
	}

	printf("published to %s\n", argv[2]);
	smolmqtt_disconnect(&m);
	return 0;
}
