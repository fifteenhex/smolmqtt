/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Shared bits for file2mqtt (the bridge) and mqttfile (the client): the wire
 * protocol, a CRC-32 and a few helpers.  Both ends stream straight to and from
 * the file a chunk at a time, so neither needs to hold a whole transfer in
 * memory - only the chunk bitmap, which is one bit per 4 KiB.
 *
 * Topics, in the same spirit as serial2mqtt's <topic>/rx and <topic>/tx:
 *
 *	<topic>/req	client -> bridge, one ASCII command per message
 *	<topic>/rsp	bridge -> client, one ASCII reply per message
 *	<topic>/in	client -> bridge, a chunk being pushed
 *	<topic>/out	bridge -> client, a chunk being pulled
 *
 * A chunk is a 4-byte big-endian index followed by up to FILEMQTT_CHUNK bytes;
 * chunk i holds the bytes at offset i * FILEMQTT_CHUNK.  Chunks are QoS 0, so
 * some may go missing: after sending them all, the sender asks `missing` and
 * resends whatever the receiver still wants, until nothing is left.
 *
 * Commands, and the replies they draw:
 *
 *	list				file <name> <size>	(zero or more)
 *					ok list <count>
 *	get <name>			... chunks on <topic>/out ...
 *					ok get <name> <size> <crc32> <chunks>
 *	put <name> <size> <crc32>	ok put ready <chunks> <chunksize>
 *	missing				missing <i> <j> ... | missing none
 *	end				ok put <name> <size> <crc32>
 *	abort				ok abort
 *
 * Anything that goes wrong answers `err <text>`.  A push lands in <name>.part
 * and is only renamed over <name> once the size and CRC both match, so an
 * interrupted transfer cannot leave a half-written file in place of a good one.
 */
#ifndef _FILEMQTT_H
#define _FILEMQTT_H

#include <stdint.h>

#define FILEMQTT_CHUNK		4096	/* payload bytes per chunk	     */
#define FILEMQTT_HDR		4	/* the big-endian chunk index	     */
#define FILEMQTT_MAX_MISSING	48	/* indices per `missing` reply	     */
#define FILEMQTT_NAME_MAX	96
#define FILEMQTT_LINE		512

/* Keep a chunk plus its topic comfortably inside the library's receive
 * buffer; smolmqtt rejects anything larger with SMOLMQTT_ERR_TOOBIG.
 */
#ifndef SMOLMQTT_RXBUF_SZ
#define SMOLMQTT_RXBUF_SZ	(FILEMQTT_CHUNK + 512)
#endif

static inline uint32_t filemqtt_be32(const uint8_t *p)
{
	return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) |
	       ((uint32_t) p[2] << 8) | p[3];
}

static inline void filemqtt_put_be32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t) (v >> 24);
	p[1] = (uint8_t) (v >> 16);
	p[2] = (uint8_t) (v >> 8);
	p[3] = (uint8_t) v;
}

/* Bit-reflected CRC-32 (the zlib/PNG one), computed without a table so the
 * static binary stays small.  Matches u-boot's `crc32`, which makes a
 * transferred kernel easy to spot-check from the boot loader.
 */
static inline uint32_t filemqtt_crc32(uint32_t crc, const uint8_t *buf,
				      uint32_t len)
{
	uint32_t i, j;

	crc = ~crc;
	for (i = 0; i < len; i++) {
		crc ^= buf[i];
		for (j = 0; j < 8; j++)
			crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t) (-(int32_t) (crc & 1)));
	}

	return ~crc;
}

/* Chunk bitmap: one bit per chunk, set as each one arrives. */
static inline void filemqtt_bit_set(uint8_t *map, uint32_t i)
{
	map[i >> 3] |= (uint8_t) (1u << (i & 7));
}

static inline int filemqtt_bit_get(const uint8_t *map, uint32_t i)
{
	return (map[i >> 3] >> (i & 7)) & 1;
}

static inline uint32_t filemqtt_chunks(uint32_t size)
{
	return (size + FILEMQTT_CHUNK - 1) / FILEMQTT_CHUNK;
}

/* Number of bytes a bitmap for `chunks` chunks needs. */
static inline uint32_t filemqtt_map_bytes(uint32_t chunks)
{
	return (chunks + 7) / 8;
}

/* Tiny parsers shared by both ends: the protocol is all space-separated
 * ASCII words and decimal or hex numbers.
 */
static inline void filemqtt_word(const char **p, char *out, uint32_t out_sz)
{
	const char *s = *p;
	uint32_t o = 0;

	while (*s == ' ')
		s++;
	while (*s && *s != ' ' && o < out_sz - 1)
		out[o++] = *s++;
	out[o] = 0;
	*p = s;
}

static inline uint32_t filemqtt_num(const char **p, int hex)
{
	const char *s = *p;
	uint32_t v = 0;

	while (*s == ' ')
		s++;
	while (*s) {
		int d;

		if (*s >= '0' && *s <= '9')
			d = *s - '0';
		else if (hex && *s >= 'a' && *s <= 'f')
			d = *s - 'a' + 10;
		else if (hex && *s >= 'A' && *s <= 'F')
			d = *s - 'A' + 10;
		else
			break;
		v = v * (uint32_t) (hex ? 16 : 10) + (uint32_t) d;
		s++;
	}
	*p = s;

	return v;
}

#endif /* _FILEMQTT_H */
