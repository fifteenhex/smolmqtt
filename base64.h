/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * base64, for carrying bytes in an MQTT payload that has to stay text.
 * Decoding skips the padding and anything else outside the alphabet.
 */
#ifndef __BASE64_H_
#define __BASE64_H_

#ifndef NOLIBC
#include <stdint.h>
#endif

static const char b64_alpha[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static inline uint32_t b64_encode(const uint8_t *in, uint32_t len, char *out)
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

static inline int b64_val(char c)
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

static inline uint32_t b64_decode(const uint8_t *in, uint32_t len, uint8_t *out)
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

#endif /* __BASE64_H_ */
