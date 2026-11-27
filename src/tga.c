/* Copyright (C) 2026 AliceHub Contributors
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

#include <stdlib.h>
#include <string.h>

#include "system4.h"
#include "system4/cg.h"
#include "system4/little_endian.h"
#include "system4/tga.h"

#define TGA_HEADER_SIZE 18

bool tga_checkfmt(const uint8_t *data)
{
	if (!data)
		return false;
	if (data[1] != 0)
		return false;
	if (data[2] != 2 && data[2] != 10)
		return false;
	if (data[16] != 24 && data[16] != 32)
		return false;
	if ((data[17] & 0xc0) != 0)
		return false;
	uint16_t w = (uint16_t)LittleEndian_getW(data, 12);
	uint16_t h = (uint16_t)LittleEndian_getW(data, 14);
	return w > 0 && h > 0 && w <= 32768 && h <= 32768;
}

bool tga_get_metrics(const uint8_t *data, size_t size, struct cg_metrics *metrics)
{
	if (!data || size < TGA_HEADER_SIZE) {
		WARNING("Data size too small for TGA header");
		return false;
	}
	if (data[16] != 24 && data[16] != 32) {
		WARNING("Unsupported TGA bpp: %u", data[16]);
		return false;
	}
	metrics->x = (uint16_t)LittleEndian_getW(data, 8);
	metrics->y = (uint16_t)LittleEndian_getW(data, 10);
	metrics->w = (uint16_t)LittleEndian_getW(data, 12);
	metrics->h = (uint16_t)LittleEndian_getW(data, 14);
	if (metrics->w <= 0 || metrics->h <= 0 || metrics->w > 32768 || metrics->h > 32768 ||
	    (uint64_t)metrics->w * metrics->h > 64u * 1024u * 1024u) {
		WARNING("Invalid TGA dimensions: %dx%d", metrics->w, metrics->h);
		return false;
	}
	metrics->bpp = data[16];
	metrics->has_pixel = true;
	metrics->has_alpha = metrics->bpp == 32 && (data[17] & 0x0f) != 0;
	metrics->pixel_pitch = metrics->w * (metrics->bpp / 8);
	metrics->alpha_pitch = 1;
	return true;
}

static inline void write_tga_pixel(uint8_t *dst, const uint8_t *src, int channels)
{
	dst[0] = src[2];
	dst[1] = src[1];
	dst[2] = src[0];
	dst[3] = (channels == 4) ? src[3] : 0xff;
}

void tga_extract(const uint8_t *data, size_t size, struct cg *cg)
{
	if (!tga_get_metrics(data, size, &cg->metrics))
		return;

	uint8_t id_len = data[0];
	uint8_t img_type = data[2];
	size_t w = (size_t)cg->metrics.w;
	size_t h = (size_t)cg->metrics.h;
	size_t channels = (size_t)(cg->metrics.bpp / 8);
	uint8_t desc = data[17];
	bool top_down = (desc & 0x20) != 0;
	bool right_to_left = (desc & 0x10) != 0;

	if (size < (size_t)TGA_HEADER_SIZE + id_len) {
		WARNING("TGA truncated in image ID");
		return;
	}

	const uint8_t *src = data + TGA_HEADER_SIZE + id_len;
	const uint8_t *end = data + size;
	size_t nr_pixels = w * h;

	uint8_t *dst = xcalloc(1, w * h * 4);

	if (img_type == 2) {
		if ((size_t)(end - src) < nr_pixels * channels) {
			WARNING("TGA pixel data truncated (have %zu, need %zu)",
				(size_t)(end - src), nr_pixels * channels);
			free(dst);
			return;
		}
		for (size_t y = 0; y < h; y++) {
			size_t dst_y = top_down ? y : (h - 1 - y);
			const uint8_t *row_src = src + y * w * channels;
			uint8_t *row_dst = dst + dst_y * w * 4;
			for (size_t x = 0; x < w; x++) {
				size_t dst_x = right_to_left ? (w - 1 - x) : x;
				write_tga_pixel(row_dst + dst_x * 4,
					row_src + x * channels, (int)channels);
			}
		}
	} else if (img_type == 10) {
		size_t idx = 0;
		while (idx < nr_pixels) {
			if (src >= end) {
				WARNING("TGA RLE data truncated");
				free(dst);
				return;
			}
			uint8_t pkt = *src++;
			size_t count = (size_t)(pkt & 0x7f) + 1;
			if (count > nr_pixels - idx) {
				free(dst);
				return;
			}
			if (pkt & 0x80) {
				if ((size_t)(end - src) < channels) {
					WARNING("TGA RLE packet truncated");
					free(dst);
					return;
				}
				for (size_t k = 0; k < count; k++, idx++) {
					size_t y = idx / w;
					size_t x = idx % w;
					size_t dst_y = top_down ? y : (h - 1 - y);
					size_t dst_x = right_to_left ? (w - 1 - x) : x;
					write_tga_pixel(dst + (dst_y * w + dst_x) * 4, src, (int)channels);
				}
				src += channels;
			} else {
				if ((size_t)(end - src) < count * channels) {
					WARNING("TGA RLE raw packet truncated");
					free(dst);
					return;
				}
				for (size_t k = 0; k < count; k++, idx++) {
					size_t y = idx / w;
					size_t x = idx % w;
					size_t dst_y = top_down ? y : (h - 1 - y);
					size_t dst_x = right_to_left ? (w - 1 - x) : x;
					write_tga_pixel(dst + (dst_y * w + dst_x) * 4, src, (int)channels);
					src += channels;
				}
			}
		}
	} else {
		free(dst);
		return;
	}

	cg->type = ALCG_TGA;
	cg->pixels = dst;
}
