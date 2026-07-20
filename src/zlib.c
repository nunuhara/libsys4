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

#include "system4/zlib.h"

#ifdef HAVE_LIBDEFLATE

#undef LIBDEFLATE_DLL
#include <libdeflate.h>

bool zlib_decompress(void *dst, size_t dst_capacity, const void *src, size_t src_len,
		size_t *actual_len_out)
{
	struct libdeflate_decompressor *dec = libdeflate_alloc_decompressor();
	if (!dec)
		return false;
	enum libdeflate_result r =
		libdeflate_zlib_decompress(dec, src, src_len, dst, dst_capacity, actual_len_out);
	libdeflate_free_decompressor(dec);
	return r == LIBDEFLATE_SUCCESS;
}

size_t zlib_compress(void *dst, size_t dst_len, const void *src, size_t src_len, int compression_level)
{
	struct libdeflate_compressor *comp =
		libdeflate_alloc_compressor(compression_level);
	if (!comp)
		return 0;
	size_t len = libdeflate_zlib_compress(comp, src, src_len, dst, dst_len);
	libdeflate_free_compressor(comp);
	return len;
}

size_t zlib_compress_bound(size_t src_len)
{
	return libdeflate_zlib_compress_bound(NULL, src_len);
}

#else // !HAVE_LIBDEFLATE

#include <zlib.h>

bool zlib_decompress(void *dst, size_t dst_capacity, const void *src, size_t src_len,
		size_t *actual_len_out)
{
	unsigned long actual_len = dst_capacity;
	if (uncompress(dst, &actual_len, src, src_len) != Z_OK)
		return false;
	*actual_len_out = actual_len;
	return true;
}

size_t zlib_compress(void *dst, size_t dst_len, const void *src, size_t src_len, int compression_level)
{
	unsigned long len = dst_len;
	if (compress2(dst, &len, src, src_len, compression_level) != Z_OK)
		return 0;
	return len;
}

size_t zlib_compress_bound(size_t src_len)
{
	return compressBound(src_len);
}

#endif // HAVE_LIBDEFLATE

bool zlib_decompress_exact(void *dst, size_t dst_len, const void *src, size_t src_len)
{
	size_t actual_len;
	return zlib_decompress(dst, dst_len, src, src_len, &actual_len)
		&& actual_len == dst_len;
}
