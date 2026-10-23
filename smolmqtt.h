// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef _SMOLMQTT_H
#define _SMOLMQTT_H

#ifndef NOLIBC
/*
 * nolibc hands all of this over with one -include, so the libc build is
 * where it has to be asked for -- including the pieces smolutils' helpers
 * expect to be there already.
 */
#include <ctype.h>
#include <dirent.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "config.h"
#include "inc/printing.h"
#include "inc/io.h"
#include "inc/net.h"
#include "inc/unaligned.h"

/*
 * A tiny single-header MQTT 3.1.1 client.
 *
 *	struct smolmqtt m = { 0 };
 *	smolmqtt_connect(&m, "192.168.3.2", 1883, "clientid");
 *	smolmqtt_publish(&m, "topic", "hi", 2, 0, false);
 *	smolmqtt_subscribe(&m, "topic", 0);
 *	smolmqtt_poll(&m, callback, priv);
 *
 * CONNECT, PUBLISH (QoS 0/1), SUBSCRIBE, PINGREQ, DISCONNECT. No auth, no
 * TLS, no QoS 2. The broker is an IPv4 dotted-quad, so no DNS.
 *
 * Everything is static inline; include it in one translation unit. The I/O
 * loops, the byte order and the address parsing are smolutils' (inc/io.h,
 * inc/unaligned.h, inc/net.h), so a checkout of that has to be on the include
 * path. Define VERBOSE before this for a line per packet.
 */

#ifndef SMOLMQTT_RXBUF_SZ
#define SMOLMQTT_RXBUF_SZ	1024
#endif

/* Error numbers, returned negated */
#define SMOLMQTT_ERR_SOCKET	1
#define SMOLMQTT_ERR_BADADDR	2
#define SMOLMQTT_ERR_CONNECT	3
#define SMOLMQTT_ERR_IO		4
#define SMOLMQTT_ERR_PROTO	5	/* unexpected reply from the broker */
#define SMOLMQTT_ERR_REFUSED	6	/* CONNACK/SUBACK said no */
#define SMOLMQTT_ERR_TOOBIG	7	/* bigger than SMOLMQTT_RXBUF_SZ */

/* Control packet types (top nibble of the fixed header) */
#define SMOLMQTT_PKT_CONNECT	0x10
#define SMOLMQTT_PKT_CONNACK	0x20
#define SMOLMQTT_PKT_PUBLISH	0x30
#define SMOLMQTT_PKT_PUBACK	0x40
#define SMOLMQTT_PKT_SUBSCRIBE	0x80
#define SMOLMQTT_PKT_SUBACK	0x90
#define SMOLMQTT_PKT_PINGREQ	0xC0
#define SMOLMQTT_PKT_PINGRESP	0xD0
#define SMOLMQTT_PKT_DISCONNECT	0xE0

/* PUBLISH flags (low nibble of the fixed header) */
#define SMOLMQTT_PUB_RETAIN	0x01
#define SMOLMQTT_PUB_QOS_SHIFT	1

struct smolmqtt {
	int fd;
	uint16_t next_packet_id;
};

/*
 * Handed to the callback for each incoming PUBLISH. The pointers are into
 * smolmqtt_poll()'s stack buffer, so they die with the call.
 */
struct smolmqtt_message {
	const char *topic;
	uint16_t topic_len;
	const uint8_t *payload;
	uint32_t payload_len;
	uint8_t qos;
	bool retain;
};

typedef void (*smolmqtt_message_cb)(struct smolmqtt *m,
				    const struct smolmqtt_message *msg,
				    void *priv);

/* Exactly len bytes, or the connection is no good any more */
static inline int __smolmqtt_read_all(struct smolmqtt *m, void *buf, int len)
{
	return read_full(m->fd, buf, len) == len ? 0 : -SMOLMQTT_ERR_IO;
}

static inline int __smolmqtt_write_all(struct smolmqtt *m, const void *buf,
				       int len)
{
	return write_full(m->fd, buf, len) ? -SMOLMQTT_ERR_IO : 0;
}

/* Give the socket up after a failure */
static inline int __smolmqtt_drop(struct smolmqtt *m, int err)
{
	close(m->fd);
	m->fd = -1;

	return err;
}

/* "Remaining length" is a base-128 varint of up to 4 bytes */
static inline int __smolmqtt_encode_len(uint32_t value, uint8_t *out)
{
	int i = 0;

	do {
		uint8_t byte = value & 0x7f;

		value >>= 7;
		if (value)
			byte |= 0x80;
		out[i++] = byte;
	} while (value && i < 4);

	return i;
}

static inline int __smolmqtt_read_len(struct smolmqtt *m, uint32_t *value)
{
	uint32_t v = 0;
	int shift = 0;
	int i;

	for (i = 0; i < 4; i++) {
		uint8_t byte;
		int ret = __smolmqtt_read_all(m, &byte, 1);

		if (ret)
			return ret;

		v |= (uint32_t) (byte & 0x7f) << shift;
		if (!(byte & 0x80)) {
			*value = v;
			return 0;
		}
		shift += 7;
	}

	return -SMOLMQTT_ERR_PROTO;
}

static inline uint8_t *__smolmqtt_put_u16(uint8_t *p, uint16_t v)
{
	put_unaligned_be16(p, v);

	return p + 2;
}

/* MQTT string: 16-bit length then the bytes */
static inline uint8_t *__smolmqtt_put_str(uint8_t *p, const char *s,
					  uint16_t len)
{
	p = __smolmqtt_put_u16(p, len);
	memcpy(p, s, len);
	return p + len;
}

static inline int smolmqtt_connect(struct smolmqtt *m, const char *broker_ip,
				   int port, const char *client_id)
{
	uint8_t hdr = SMOLMQTT_PKT_CONNECT;
	struct sockaddr_in addr;
	struct in_addr in;
	uint8_t pkt[256];
	uint8_t *p = pkt;
	uint8_t rem[4];
	uint8_t connack[4];
	int nrem;
	int ret;

	m->next_packet_id = 1;

	if (!inet_aton(broker_ip, &in))
		return -SMOLMQTT_ERR_BADADDR;

	m->fd = socket(AF_INET, SOCK_STREAM, 0);
	if (m->fd < 0)
		return -SMOLMQTT_ERR_SOCKET;

	sockaddr_in_init(&addr, in.s_addr, port);
	if (connect(m->fd, (struct sockaddr *) &addr, sizeof(addr)) < 0)
		return __smolmqtt_drop(m, -SMOLMQTT_ERR_CONNECT);

	p = __smolmqtt_put_str(p, "MQTT", 4);
	*p++ = 0x04;			/* protocol level 4 = 3.1.1 */
	*p++ = 0x02;			/* clean session */
	p = __smolmqtt_put_u16(p, 0);	/* keepalive disabled */
	p = __smolmqtt_put_str(p, client_id, strlen(client_id));

	nrem = __smolmqtt_encode_len((uint32_t) (p - pkt), rem);

	if (__smolmqtt_write_all(m, &hdr, 1) ||
	    __smolmqtt_write_all(m, rem, nrem) ||
	    __smolmqtt_write_all(m, pkt, p - pkt))
		return __smolmqtt_drop(m, -SMOLMQTT_ERR_IO);

	ret = __smolmqtt_read_all(m, connack, sizeof(connack));
	if (ret)
		return __smolmqtt_drop(m, ret);

	if (connack[0] != SMOLMQTT_PKT_CONNACK || connack[1] != 0x02)
		return __smolmqtt_drop(m, -SMOLMQTT_ERR_PROTO);

	if (connack[3] != 0)
		return __smolmqtt_drop(m, -SMOLMQTT_ERR_REFUSED);

	verbose("smolmqtt: connected to %s:%d as '%s'\n", broker_ip, port,
		client_id);

	return 0;
}

static inline void smolmqtt_disconnect(struct smolmqtt *m)
{
	uint8_t pkt[2] = { SMOLMQTT_PKT_DISCONNECT, 0x00 };

	if (m->fd >= 0) {
		__smolmqtt_write_all(m, pkt, sizeof(pkt));
		close(m->fd);
		m->fd = -1;
	}
}

/* qos 0 or 1; qos 1 blocks for the PUBACK */
static inline int smolmqtt_publish(struct smolmqtt *m, const char *topic,
				   const void *payload, uint32_t payload_len,
				   uint8_t qos, bool retain)
{
	uint8_t hdr[5];
	uint8_t vh[256];
	uint8_t *p = vh;
	uint16_t packet_id = 0;
	uint32_t varlen;
	int nrem, ret;

	p = __smolmqtt_put_str(p, topic, strlen(topic));
	if (qos) {
		packet_id = m->next_packet_id++;
		if (!m->next_packet_id)
			m->next_packet_id = 1;
		p = __smolmqtt_put_u16(p, packet_id);
	}

	varlen = (uint32_t) (p - vh) + payload_len;

	hdr[0] = SMOLMQTT_PKT_PUBLISH | (uint8_t) (qos << SMOLMQTT_PUB_QOS_SHIFT)
	       | (retain ? SMOLMQTT_PUB_RETAIN : 0);
	nrem = __smolmqtt_encode_len(varlen, hdr + 1);

	if (__smolmqtt_write_all(m, hdr, 1 + nrem) ||
	    __smolmqtt_write_all(m, vh, p - vh) ||
	    __smolmqtt_write_all(m, payload, payload_len))
		return -SMOLMQTT_ERR_IO;

	if (qos == 1) {
		uint8_t puback[4];

		ret = __smolmqtt_read_all(m, puback, sizeof(puback));
		if (ret)
			return ret;
		if (puback[0] != SMOLMQTT_PKT_PUBACK ||
		    get_unaligned_be16(puback + 2) != packet_id)
			return -SMOLMQTT_ERR_PROTO;
	}

	verbose("smolmqtt: published %u bytes to '%s' (qos %u)\n", payload_len,
		topic, qos);

	return 0;
}

/* Subscribe to one topic filter; blocks for the SUBACK */
static inline int smolmqtt_subscribe(struct smolmqtt *m, const char *topic,
				     uint8_t qos)
{
	uint8_t hdr[5];
	uint8_t vh[256];
	uint8_t *p = vh;
	uint16_t packet_id;
	uint8_t suback[5];
	int nrem, ret;

	packet_id = m->next_packet_id++;
	if (!m->next_packet_id)
		m->next_packet_id = 1;

	p = __smolmqtt_put_u16(p, packet_id);
	p = __smolmqtt_put_str(p, topic, strlen(topic));
	*p++ = qos;

	/* SUBSCRIBE is one of the types with mandatory flags 0b0010 */
	hdr[0] = SMOLMQTT_PKT_SUBSCRIBE | 0x02;
	nrem = __smolmqtt_encode_len((uint32_t) (p - vh), hdr + 1);

	if (__smolmqtt_write_all(m, hdr, 1 + nrem) ||
	    __smolmqtt_write_all(m, vh, p - vh))
		return -SMOLMQTT_ERR_IO;

	ret = __smolmqtt_read_all(m, suback, sizeof(suback));
	if (ret)
		return ret;

	if (suback[0] != SMOLMQTT_PKT_SUBACK ||
	    get_unaligned_be16(suback + 2) != packet_id)
		return -SMOLMQTT_ERR_PROTO;
	if (suback[4] & 0x80)
		return -SMOLMQTT_ERR_REFUSED;

	verbose("smolmqtt: subscribed to '%s' (granted qos %u)\n", topic,
		suback[4]);

	return 0;
}

/*
 * Read one control packet; if it is a PUBLISH, hand it to cb. Blocks until a
 * packet arrives. Other packet types are consumed and ignored.
 */
static inline int smolmqtt_poll(struct smolmqtt *m, smolmqtt_message_cb cb,
				void *priv)
{
	uint8_t fixed;
	uint32_t remlen;
	uint8_t buf[SMOLMQTT_RXBUF_SZ];
	int ret;

	ret = __smolmqtt_read_all(m, &fixed, 1);
	if (ret)
		return ret;

	ret = __smolmqtt_read_len(m, &remlen);
	if (ret)
		return ret;

	if (remlen > sizeof(buf))
		return -SMOLMQTT_ERR_TOOBIG;

	if (remlen) {
		ret = __smolmqtt_read_all(m, buf, remlen);
		if (ret)
			return ret;
	}

	if ((fixed & 0xf0) == SMOLMQTT_PKT_PUBLISH) {
		struct smolmqtt_message msg = { 0 };
		uint8_t qos = (fixed >> SMOLMQTT_PUB_QOS_SHIFT) & 0x03;
		uint32_t off;
		uint16_t tlen;

		if (remlen < 2)
			return -SMOLMQTT_ERR_PROTO;

		tlen = get_unaligned_be16(buf);
		off = 2 + tlen;
		if (off > remlen)
			return -SMOLMQTT_ERR_PROTO;

		msg.topic = (const char *) (buf + 2);
		msg.topic_len = tlen;
		msg.qos = qos;
		msg.retain = fixed & SMOLMQTT_PUB_RETAIN;

		if (qos > 0) {
			uint16_t pid;

			if (off + 2 > remlen)
				return -SMOLMQTT_ERR_PROTO;
			pid = get_unaligned_be16(buf + off);
			off += 2;

			if (qos == 1) {
				uint8_t ack[4] = { SMOLMQTT_PKT_PUBACK, 0x02 };

				put_unaligned_be16(ack + 2, pid);
				if (__smolmqtt_write_all(m, ack, sizeof(ack)))
					return -SMOLMQTT_ERR_IO;
			}
		}

		msg.payload = buf + off;
		msg.payload_len = remlen - off;

		if (cb)
			cb(m, &msg, priv);
	}

	return 0;
}

static inline int smolmqtt_ping(struct smolmqtt *m)
{
	uint8_t req[2] = { SMOLMQTT_PKT_PINGREQ, 0x00 };
	uint8_t resp[2];
	int ret;

	if (__smolmqtt_write_all(m, req, sizeof(req)))
		return -SMOLMQTT_ERR_IO;

	ret = __smolmqtt_read_all(m, resp, sizeof(resp));
	if (ret)
		return ret;
	if (resp[0] != SMOLMQTT_PKT_PINGRESP)
		return -SMOLMQTT_ERR_PROTO;

	return 0;
}

#endif /* _SMOLMQTT_H */
