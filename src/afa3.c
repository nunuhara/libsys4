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
#include "little_endian.h"
#include "system4.h"
#include "system4/afa.h"
#include "system4/archive.h"
#include "system4/buffer.h"
#include "system4/string.h"
#include "system4/zlib.h"

typedef struct string *(*string_conv_fun)(const char*,size_t);

struct afa3_rnd {
	uint32_t state[521];
	int current;
};

/*
 * Munge PRNG state.
 */
static void rnd_shuffle(struct afa3_rnd *rnd)
{
	for (int i = 0; i < 32; i += 4) {
		rnd->state[i  ] ^= rnd->state[i+489];
		rnd->state[i+1] ^= rnd->state[i+490];
		rnd->state[i+2] ^= rnd->state[i+491];
		rnd->state[i+3] ^= rnd->state[i+492];
	}
	for (int i = 32; i < 521; i += 3) {
		rnd->state[i  ] ^= rnd->state[i - 32];
		rnd->state[i+1] ^= rnd->state[i - 31];
		rnd->state[i+2] ^= rnd->state[i - 30];
	}
}

/*
 * Initialize the PRNG.
 */
static void rnd_init(struct afa3_rnd *rnd, uint32_t seed)
{
	uint32_t val = 0;
	for (int i = 0; i < 17; i++) {
		for (int j = 0; j < 32; j++) {
			seed = 1566083941u * seed + 1;
			val = (seed & 0x80000000) | (val >> 1);
		}
		rnd->state[i] = val;
	}
	rnd->state[16] = rnd->state[15] ^ (rnd->state[0] >> 9) ^ (rnd->state[16] << 23);
	for (int i = 17; i < 521; i++) {
		rnd->state[i] = rnd->state[i-1] ^ (rnd->state[i-16] >> 9) ^ (rnd->state[i-17] << 23);
	}
	rnd_shuffle(rnd);
	rnd_shuffle(rnd);
	rnd_shuffle(rnd);
	rnd_shuffle(rnd);
	rnd->current = -1;
}

/*
 * Get the next pseudo-random number.
 */
static uint32_t rnd_get_next(struct afa3_rnd *rnd)
{
	rnd->current++;
	if (rnd->current >= 521) {
		rnd_shuffle(rnd);
		rnd->current = 0;
	}
	return rnd->state[rnd->current];
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
static bool bs_read_u32(struct bitstream *bs, uint32_t *out)
{
	int b0 = bs_read_bits(bs, 8);
	int b1 = bs_read_bits(bs, 8);
	int b2 = bs_read_bits(bs, 8);
	int b3 = bs_read_bits(bs, 8);
	if (b0 < 0 || b1 < 0 || b2 < 0 || b3 < 0)
		return false;
	*out = ((uint32_t)b3 << 24) | ((uint32_t)b2 << 16) | ((uint32_t)b1 << 8) | (uint32_t)b0;
	return true;
}

/*
 * Dictionary for string decoding.
 */
struct afa3_dict {
	uint8_t *bytes;
	size_t size;
};

static void free_dict(struct afa3_dict *dict)
{
	free(dict->bytes);
	dict->bytes = NULL;
	dict->size = 0;
}

/*
 * Read the string decoding dictionary.
 * Encrypted via a PRNG seeded by the dictionary size.
 */
static bool read_dict(struct bitstream *bs, struct afa3_dict *dict)
{
	free_dict(dict);

	uint32_t raw_size = 0;
	if (!bs_read_u32(bs, &raw_size) || raw_size > 64u * 1024u * 1024u) {
		return false;
	}
	dict->size = raw_size;
	dict->bytes = xmalloc(dict->size ? dict->size : 1);

	struct afa3_rnd rnd;
	rnd_init(&rnd, (uint32_t)dict->size);
	for (unsigned dst = 0; dst < dict->size; dst++) {
		int count = (int)(rnd_get_next(&rnd) & 3u);
		int skipped = bs_read_bits(bs, count+1);
		if (skipped == -1) {
			goto err;
		}
		rnd_get_next(&rnd);

		int v = bs_read_bits(bs, 8);
		if (v == -1) {
			goto err;
		}
		dict->bytes[dst] = (uint8_t)v;
	}
	return true;
err:
	free_dict(dict);
	return false;
}

/*
 * Read an encrypted string. The data is double-encrypted: once via a PRNG
 * seeded by the string length, and again via the encrypted dictionary (above).
 * This function decrypts the first layer.
 */
static uint16_t *afa3_read_encrypted_chars(struct bitstream *bs, size_t *size)
{
	uint32_t buf_size = 0;
	if (!bs_read_u32(bs, &buf_size) || buf_size > 64u * 1024u * 1024u) {
		return NULL;
	}
	uint16_t *buf = xmalloc(buf_size ? (size_t)buf_size * 2 : 2);

	struct afa3_rnd rnd;
	rnd_init(&rnd, buf_size);
	for (unsigned dst = 0; dst < buf_size; dst++) {
		int count = (int)(rnd_get_next(&rnd) & 3u);
		int skipped = bs_read_bits(bs, count+1);
		if (skipped == -1) {
			goto err;
		}
		rnd_get_next(&rnd);

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
static char *afa3_decrypt_string(const struct afa3_dict *dict, uint16_t *chars, size_t size)
{
	if (!dict->bytes && size > 0)
		return NULL;
	char *buf = xmalloc(size+1);
	for (size_t i = 0; i < size; i++) {
		if (chars[i] >= dict->size) {
			free(buf);
			return NULL;
		}
		buf[i] = (char)(dict->bytes[chars[i]] ^ 0xa4);
	}
	buf[size] = '\0';
	return buf;
}

/*
 * Read the metadata for a single file.
 */
static bool afa3_read_entry(struct bitstream *bs, const struct afa3_dict *dict, struct afa_entry *entry, int *error, string_conv_fun conv)
{
	size_t size;
	uint16_t *chars = NULL;
	char *name = NULL;

	chars = afa3_read_encrypted_chars(bs, &size);
	if (!chars) {
		*error = ARCHIVE_BAD_ARCHIVE_ERROR;
		goto err;
	}

	name = afa3_decrypt_string(dict, chars, size);
	if (!name) {
		*error = ARCHIVE_BAD_ARCHIVE_ERROR;
		goto err;
	}

	uint32_t uk0, uk1, off, fsize;
	if (!bs_read_u32(bs, &uk0) || !bs_read_u32(bs, &uk1) ||
	    !bs_read_u32(bs, &off) || !bs_read_u32(bs, &fsize)) {
		*error = ARCHIVE_BAD_ARCHIVE_ERROR;
		goto err;
	}

	entry->name = conv(name, strlen(name));
	entry->unknown0 = uk0;
	entry->unknown1 = uk1;
	entry->off = off;
	entry->size = fsize;
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
	struct afa3_dict dict = {0};
	uint32_t index_size = LittleEndian_getDW((uint8_t*)hdr, 4);

	struct bitstream bs;
	bs_init_file(&bs, f, 12);
	if (bs_read_bits(&bs, 1) == -1 || !read_dict(&bs, &dict)) {
		*error = ARCHIVE_BAD_ARCHIVE_ERROR;
		goto err;
	}
	uint32_t packed_size = 0, unpacked_size = 0;
	if (!bs_read_u32(&bs, &packed_size) || !bs_read_u32(&bs, &unpacked_size) ||
	    packed_size > 256u * 1024u * 1024u || unpacked_size > 256u * 1024u * 1024u) {
		*error = ARCHIVE_BAD_ARCHIVE_ERROR;
		goto err;
	}

	// read zlib compressed data
	packed = xmalloc(packed_size ? packed_size : 1);
	for (uint32_t i = 0; i < packed_size; i++) {
		int b = bs_read_bits(&bs, 8);
		if (b < 0) {
			*error = ARCHIVE_BAD_ARCHIVE_ERROR;
			goto err;
		}
		packed[i] = (uint8_t)b;
	}

	// decompress
	unpacked = xmalloc(unpacked_size ? unpacked_size : 1);
	if (!zlib_decompress_exact(unpacked, unpacked_size, packed, packed_size)) {
		*error = ARCHIVE_BAD_ARCHIVE_ERROR;
		goto err;
	}

	bs_init_buffer(&bs, unpacked, unpacked_size);
	if (bs_read_bits(&bs, 1) == -1) {
		*error = ARCHIVE_BAD_ARCHIVE_ERROR;
		goto err;
	}
	uint32_t nr_files = 0;
	if (!bs_read_u32(&bs, &nr_files) || nr_files > unpacked_size / 16 + 1) {
		*error = ARCHIVE_BAD_ARCHIVE_ERROR;
		goto err;
	}
	ar->nr_files = nr_files;
	ar->files = xcalloc(ar->nr_files ? ar->nr_files : 1, sizeof(struct afa_entry));
	for (uint32_t i = 0; i < ar->nr_files; i++) {
		if (bs_read_bits(&bs, 2) == -1) {
			ar->nr_files = i;
			break;
		}
		if (!afa3_read_entry(&bs, &dict, &ar->files[i], error, conv)) {
			for (uint32_t j = 0; j < i; j++) {
				free_string(ar->files[j].name);
			}
			free(ar->files);
			ar->files = NULL;
			ar->nr_files = 0;
			goto err;
		}
		ar->files[i].no = i;
	}

	ar->version = 3;
	ar->data_start = index_size + 8;
	ar->compressed_size = packed_size;
	ar->uncompressed_size = unpacked_size;

	free_dict(&dict);
	free(packed);
	free(unpacked);
	return true;
err:
	free_dict(&dict);
	free(packed);
	free(unpacked);
	return false;
}
