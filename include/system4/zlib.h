/* Copyright (C) 2026 kichikuou <KichikuouChrome@gmail.com>
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

#ifndef SYSTEM4_ZLIB_H
#define SYSTEM4_ZLIB_H

#include <stdbool.h>
#include <stddef.h>

#define ZLIB_BEST_SPEED 1
#define ZLIB_DEFAULT_COMPRESSION 6
#define ZLIB_BEST_COMPRESSION 9

// Decompresses a block of data compressed in the zlib format (RFC 1950).
// The decompressed size is written to actual_len_out on success.
bool zlib_decompress(void *dst, size_t dst_capacity, const void *src, size_t src_len,
		size_t *actual_len_out);

// Decompresses a block of data and fails if its size differs from dst_len.
bool zlib_decompress_exact(void *dst, size_t dst_len, const void *src, size_t src_len);

// Compresses a block of data using the zlib format (RFC 1950).
// Returns the size of the compressed data if successful, 0 otherwise.
size_t zlib_compress(void *dst, size_t dst_len, const void *src, size_t src_len, int compression_level);

// Calculates the maximum length of data after compression.
size_t zlib_compress_bound(size_t src_len);

#endif /* SYSTEM4_ZLIB_H */
