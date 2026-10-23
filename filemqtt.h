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

#ifndef NOLIBC
#include <ctype.h>
#include <endian.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#endif

#include "config.h"
#include "inc/printing.h"
#include "inc/numbers.h"
#include "inc/strings.h"
#include "inc/unaligned.h"

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

/* The chunk index; inc/unaligned.h reads it but has no put_unaligned_be32() */
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

/* The next field of a line as a number, if there is one and it is one. The
 * words of a line come off with smolutils' next_field().
 */
static inline bool filemqtt_next_number(char **p, int base, uint32_t *out)
{
	char *f = next_field(p);
	unsigned long v;

	if (!f || !parse_ul(f, base, &v) || v > UINT32_MAX)
		return false;
	*out = v;

	return true;
}

#endif /* _FILEMQTT_H */
