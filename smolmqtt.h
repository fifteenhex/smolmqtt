// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef _SMOLMQTT_H
#define _SMOLMQTT_H

#ifndef NOLIBC
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#endif

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
 * Everything is static inline; include it in one translation unit.
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

#ifdef SMOLMQTT_DEBUG
#include <stdio.h>
#define __smolmqtt_debug(fmt, ...) printf("smolmqtt: " fmt, ##__VA_ARGS__)
#else
#define __smolmqtt_debug(fmt, ...) do { } while (0)
#endif

static inline int __smolmqtt_write_all(struct smolmqtt *m, const void *buf,
				       size_t len)
{
	const uint8_t *p = buf;
	size_t done = 0;

	while (done < len) {
		ssize_t n = write(m->fd, p + done, len - done);

		if (n > 0)
			done += (size_t) n;
		else if (n < 0 && errno == EINTR)
			continue;
		else
			return -SMOLMQTT_ERR_IO;
	}

	return 0;
}

static inline int __smolmqtt_read_all(struct smolmqtt *m, void *buf, size_t len)
{
	uint8_t *p = buf;
	size_t done = 0;

	while (done < len) {
		ssize_t n = read(m->fd, p + done, len - done);

		if (n > 0)
			done += (size_t) n;
		else if (n < 0 && errno == EINTR)
			continue;
		else
			return -SMOLMQTT_ERR_IO;
	}

	return 0;
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
	*p++ = (uint8_t) (v >> 8);
	*p++ = (uint8_t) (v & 0xff);
	return p;
}

/* MQTT string: 16-bit length then the bytes */
static inline uint8_t *__smolmqtt_put_str(uint8_t *p, const char *s,
					  uint16_t len)
{
	p = __smolmqtt_put_u16(p, len);
	memcpy(p, s, len);
	return p + len;
}

static inline uint16_t __smolmqtt_strlen(const char *s)
{
	uint16_t n = 0;

	while (s[n])
		n++;

	return n;
}

static inline int __smolmqtt_parse_ipv4(const char *s, uint32_t *out)
{
	uint32_t addr = 0;
	int octet = 0, ndigits = 0, parts = 0;

	for (;; s++) {
		if (*s >= '0' && *s <= '9') {
			octet = octet * 10 + (*s - '0');
			if (octet > 255)
				return -SMOLMQTT_ERR_BADADDR;
			ndigits++;
		} else if (*s == '.' || *s == '\0') {
			if (!ndigits)
				return -SMOLMQTT_ERR_BADADDR;
			addr = (addr << 8) | (uint32_t) octet;
			parts++;
			octet = 0;
			ndigits = 0;
			if (*s == '\0')
				break;
		} else {
			return -SMOLMQTT_ERR_BADADDR;
		}
	}

	if (parts != 4)
		return -SMOLMQTT_ERR_BADADDR;

	*out = htonl(addr);
	return 0;
}

static inline int smolmqtt_connect(struct smolmqtt *m, const char *broker_ip,
				   int port, const char *client_id)
{
	struct sockaddr_in addr = { 0 };
	uint8_t pkt[256];
	uint8_t *p = pkt;
	uint16_t cid_len = __smolmqtt_strlen(client_id);
	uint8_t rem[4];
	uint8_t connack[4];
	uint32_t addr_be;
	int nrem;
	int ret;

	m->next_packet_id = 1;

	ret = __smolmqtt_parse_ipv4(broker_ip, &addr_be);
	if (ret)
		return ret;

	m->fd = socket(AF_INET, SOCK_STREAM, 0);
	if (m->fd < 0)
		return -SMOLMQTT_ERR_SOCKET;

	addr.sin_family = AF_INET;
	addr.sin_port = htons((uint16_t) port);
	addr.sin_addr.s_addr = addr_be;

	if (connect(m->fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
		close(m->fd);
		m->fd = -1;
		return -SMOLMQTT_ERR_CONNECT;
	}

	p = __smolmqtt_put_str(p, "MQTT", 4);
	*p++ = 0x04;			/* protocol level 4 = 3.1.1 */
	*p++ = 0x02;			/* clean session */
	p = __smolmqtt_put_u16(p, 0);	/* keepalive disabled */
	p = __smolmqtt_put_str(p, client_id, cid_len);

	nrem = __smolmqtt_encode_len((uint32_t) (p - pkt), rem);

	{
		uint8_t hdr = SMOLMQTT_PKT_CONNECT;

		if (__smolmqtt_write_all(m, &hdr, 1) ||
		    __smolmqtt_write_all(m, rem, nrem) ||
		    __smolmqtt_write_all(m, pkt, (size_t) (p - pkt))) {
			close(m->fd);
			m->fd = -1;
			return -SMOLMQTT_ERR_IO;
		}
	}

	ret = __smolmqtt_read_all(m, connack, sizeof(connack));
	if (ret) {
		close(m->fd);
		m->fd = -1;
		return ret;
	}

	if (connack[0] != SMOLMQTT_PKT_CONNACK || connack[1] != 0x02) {
		close(m->fd);
		m->fd = -1;
		return -SMOLMQTT_ERR_PROTO;
	}

	if (connack[3] != 0) {
		close(m->fd);
		m->fd = -1;
		return -SMOLMQTT_ERR_REFUSED;
	}

	__smolmqtt_debug("connected to %s:%d as '%s'\n", broker_ip, port,
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

#endif /* _SMOLMQTT_H */
