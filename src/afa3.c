/* Copyright (C) 2019 Nunuhara Cabbage <nunuhara@haniwa.technology>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://gnu.org/licenses/>.
 */

// Adapted from GARbro source code
//
// Copyright (C) 2016-2018 by morkt
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to
// deal in the Software without restriction, including without limitation the
// rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
// sell copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
// IN THE SOFTWARE.
//

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <zlib.h>
#include "little_endian.h"
#include "system4.h"
#include "system4/afa.h"
#include "system4/archive.h"
#include "system4/buffer.h"
#include "system4/string.h"

typedef struct string *(*string_conv_fun)(const char*,size_t);

static struct {
	uint32_t state[521];
	int current;
} rnd;

/*
 * Munge PRNG state.
 */
static void rnd_shuffle(void)
{
	for (int i = 0; i < 32; i += 4) {
		rnd.state[i  ] ^= rnd.state[i+489];
		rnd.state[i+1] ^= rnd.state[i+490];
		rnd.state[i+2] ^= rnd.state[i+491];
		rnd.state[i+3] ^= rnd.state[i+492];
	}
	for (int i = 32; i < 521; i += 3) {
		rnd.state[i  ] ^= rnd.state[i - 32];
		rnd.state[i+1] ^= rnd.state[i - 31];
		rnd.state[i+2] ^= rnd.state[i - 30];
	}
}

/*
 * Initialize the PRNG.
 */
static void rnd_init(uint32_t seed)
{
	uint32_t val = 0;
	for (int i = 0; i < 17; i++) {
		for (int j = 0; j < 32; j++) {
			seed = 1566083941u * seed + 1;
			val = (seed & 0x80000000) | (val >> 1);
		}
		rnd.state[i] = val;
	}
	rnd.state[16] = rnd.state[15] ^ (rnd.state[0] >> 9) ^ (rnd.state[16] << 23);
	for (int i = 17; i < 521; i++) {
		rnd.state[i] = rnd.state[i-1] ^ (rnd.state[i-16] >> 9) ^ (rnd.state[i-17] << 23);
	}
	rnd_shuffle();
	rnd_shuffle();
	rnd_shuffle();
	rnd_shuffle();
	rnd.current = -1;
}

/*
 * Get the next pseudo-random number.
 */
static uint32_t rnd_get_next(void)
{
	rnd.current++;
	if (rnd.current >= 521) {
		rnd_shuffle();
		rnd.current = 0;
	}
	return rnd.state[rnd.current];
}

/*
 * Stream abstraction for reading non-byte-aligned data.
 * Can be backed by either a FILE* or a memory resident buffer.
 */
struct bitstream {
	enum {
		BITSTREAM_FILE,
		BITSTREAM_BUFFER
	} type;
	union {
		FILE *f;
		struct buffer b;
	};
	int nr_cached;
	uint32_t cache;
};

/*
 * Initialize a bitstream from a FILE*.
 */
static void bs_init_file(struct bitstream *bs, FILE *f, long off)
{
	bs->type = BITSTREAM_FILE;
	bs->f = f;
	fseek(f, off, SEEK_SET);
	bs->nr_cached = 0;
	bs->cache = 0;
}

/*
 * Initialize a bitsream from a buffer.
 */
static void bs_init_buffer(struct bitstream *bs, uint8_t *buf, size_t size)
{
	bs->type = BITSTREAM_BUFFER;
	buffer_init(&bs->b, buf, size);
	bs->nr_cached = 0;
	bs->cache = 0;
}

/*
 * Read the next byte from a bitstream (internal).
 */
static int _bs_next_byte(struct bitstream *bs)
{
	if (bs->type == BITSTREAM_FILE) {
		uint8_t b;
		if (fread(&b, 1, 1, bs->f) != 1) {
			return -1;
		}
		return b;
	}
	if (buffer_remaining(&bs->b) < 1) {
		return -1;
	}
	return buffer_read_u8(&bs->b);
}

/*
 * Read an arbitrary number of bits from a bitstream (MSB order).
 */
static int bs_read_bits(struct bitstream *bs, int count)
{
	while (bs->nr_cached < count) {
		int b = _bs_next_byte(bs);
		if (b < 0) {
			return -1;
		}
		bs->cache = (bs->cache << 8) | (uint8_t)b;
		bs->nr_cached += 8;
	}

	int mask = (1 << count) - 1;
	bs->nr_cached -= count;
	return (bs->cache >> bs->nr_cached) & mask;
}

/*
 * Read a 32-bit little endian integer from a bitstream.
 */
static int bs_read_int32(struct bitstream *bs)
{
	int b0 = bs_read_bits(bs, 8);
	int b1 = bs_read_bits(bs, 8);
	int b2 = bs_read_bits(bs, 8);
	int b3 = bs_read_bits(bs, 8);
	return (b3 << 24) | (b2 << 16) | (b1 << 8) | b0;
}

/*
 * Dictionary for string decoding.
 */
struct {
	uint8_t *bytes;
	size_t size;
} dict;

/*
 * Read the string decoding dictionary.
 * Encrypted via a PRNG seeded by the dictionary size.
 */
static void read_dict(struct bitstream *bs)
{
	dict.size = bs_read_int32(bs);
	dict.bytes = xmalloc(dict.size);

	rnd_init(dict.size);
	for (unsigned dst = 0; dst < dict.size; dst++) {
		int count = (int)rnd_get_next() & 3;
		int skipped = bs_read_bits(bs, count+1);
		if (skipped == -1) {
			goto err;
		}
		rnd_get_next();

		int v = bs_read_bits(bs, 8);
		if (v == -1) {
			goto err;
		}
		dict.bytes[dst] = (uint8_t)v;
	}
	return;
err:
	free(dict.bytes);
}

/*
 * Read an encrypted string. The data is double-encrypted: once via a PRNG
 * seeded by the string length, and again via the encrypted dictionary (above).
 * This function decrypts the first layer.
 */
static uint16_t *afa3_read_encrypted_chars(struct bitstream *bs, size_t *size)
{
	uint32_t buf_size = bs_read_int32(bs);
	uint16_t *buf = xmalloc(buf_size * 2);

	rnd_init(buf_size);
	for (unsigned dst = 0; dst < buf_size; dst++) {
		int count = (int)rnd_get_next() & 3;
		int skipped = bs_read_bits(bs, count+1);
		if (skipped == -1) {
			goto err;
		}
		rnd_get_next();

		int lo = bs_read_bits(bs, 8);
		int hi = bs_read_bits(bs, 8);
		if (lo == -1 || hi == -1) {
			goto err;
		}
		buf[dst] = (uint16_t)(lo | (hi << 8));
	}

	*size = buf_size;
	return buf;
err:
	free(buf);
	return NULL;
}

/*
 * Decrypt an encrypted string via the dictionary.
 */
static char *afa3_decrypt_string(uint16_t *chars, size_t size)
{
	char *buf = xmalloc(size+1);
	for (unsigned i = 0; i < size; i++) {
		if (chars[i] >= dict.size) {
			free(buf);
			return NULL;
		}
		buf[i] = (char)(dict.bytes[chars[i]] ^ 0xa4);
	}
	buf[size] = '\0';
	return buf;
}

/*
 * Read the metadata for a single file.
 */
static bool afa3_read_entry(struct bitstream *bs, struct afa_entry *entry, int *error, string_conv_fun conv)
{
	size_t size;
	uint16_t *chars = NULL;
	char *name = NULL;

	chars = afa3_read_encrypted_chars(bs, &size);
	if (!chars) {
		*error = ARCHIVE_BAD_ARCHIVE_ERROR;
		goto err;
	}

	name = afa3_decrypt_string(chars, size);
	if (!name) {
		*error = ARCHIVE_BAD_ARCHIVE_ERROR;
		goto err;
	}

	entry->name = conv(name, strlen(name));
	entry->unknown0 = bs_read_int32(bs);
	entry->unknown1 = bs_read_int32(bs);
	entry->off = bs_read_int32(bs);
	entry->size = bs_read_int32(bs);
	free(chars);
	free(name);
	return true;
err:
	free(chars);
	free(name);
	return false;
}

/*
 * Read the archive metadata.
 */
bool afa3_read_metadata(char *hdr, FILE *f, struct afa_archive *ar, int *error, string_conv_fun conv)
{
	uint8_t *packed = NULL;
	uint8_t *unpacked = NULL;
	uint32_t index_size = LittleEndian_getDW((uint8_t*)hdr, 4);

	struct bitstream bs;
	bs_init_file(&bs, f, 12);
	bs_read_bits(&bs, 1); // skip first bit (obfuscation)
	read_dict(&bs);
	unsigned long packed_size = bs_read_int32(&bs);
	unsigned long unpacked_size = bs_read_int32(&bs);

	// read zlib compressed data
	packed = xmalloc(packed_size);
	for (unsigned i = 0; i < packed_size; i++) {
		packed[i] = (uint8_t)bs_read_bits(&bs, 8);
	}

	// decompress
	unpacked = xmalloc(unpacked_size);
	if (uncompress(unpacked, &unpacked_size, packed, packed_size) != Z_OK) {
		*error = ARCHIVE_BAD_ARCHIVE_ERROR;
		goto err;
	}

	bs_init_buffer(&bs, unpacked, unpacked_size);
	bs_read_bits(&bs, 1); // skip first bit (obfuscation)
	ar->nr_files = bs_read_int32(&bs);
	ar->files = xcalloc(ar->nr_files, sizeof(struct afa_entry));
	for (unsigned i = 0; i < ar->nr_files; i++) {
		if (bs_read_bits(&bs, 2) == -1)
			break;
		if (!afa3_read_entry(&bs, &ar->files[i], error, conv)) {
			free(ar->files);
			goto err;
		}
		ar->files[i].no = i;
	}

	ar->version = 3;
	ar->data_start = index_size + 8;
	ar->compressed_size = packed_size;
	ar->uncompressed_size = unpacked_size;

	free(packed);
	free(unpacked);
	return true;
err:
	free(packed);
	free(unpacked);
	return false;
}
