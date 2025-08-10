/* Copyright (C) 2025 kichikuou <KichikuouChrome@gmail.com>
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
#include "system4/rou.h"
#include "system4/little_endian.h"

#define ROU_HEADER_SIZE 0x44

bool rou_checkfmt(const uint8_t *data)
{
	return data && memcmp(data, "ROU\0", 4) == 0;
}

void rou_get_metrics(const uint8_t *data, size_t size, struct cg_metrics *metrics)
{
	if (size < ROU_HEADER_SIZE) {
		WARNING("Data size too small for ROU header");
		return;
	}
	metrics->w = LittleEndian_getDW(data, 0x14);
	metrics->h = LittleEndian_getDW(data, 0x18);
	metrics->bpp = LittleEndian_getDW(data, 0x1c);
	metrics->has_pixel = true;
	metrics->has_alpha = (LittleEndian_getDW(data, 0x28) > 0);
	metrics->pixel_pitch = metrics->w * (metrics->has_alpha ? 4 : 3);
	metrics->alpha_pitch = 1;
}

void rou_extract(const uint8_t *data, size_t size, struct cg *cg)
{
	uint32_t header_size = LittleEndian_getDW(data, 8);
	rou_get_metrics(data, size, &cg->metrics);
	uint32_t width = cg->metrics.w;
	uint32_t height = cg->metrics.h;
	uint32_t pixels_size = LittleEndian_getDW(data, 0x24);
	uint32_t alpha_size = LittleEndian_getDW(data, 0x28);

	if (size != header_size + pixels_size + alpha_size) {
		WARNING("ROU size does not match expected size");
		return;
	}
	if (pixels_size && pixels_size != width * height * 3) {
		WARNING("ROU: Unexpected pixel size");
		return;
	}
	if (alpha_size && alpha_size != width * height) {
		WARNING("ROU: Unexpected alpha size");
		return;
	}
	if (!pixels_size && !alpha_size) {
		WARNING("ROU: No pixel or alpha data found");
		return;
	}

	const uint8_t *pixels = data + header_size;
	cg->type = ALCG_ROU;
	uint8_t *dst = cg->pixels = xcalloc(1, width * height * 4);

	if (alpha_size == 0) {
		for (uint32_t i = 0; i < width * height; i++) {
			uint8_t b = *pixels++;
			uint8_t g = *pixels++;
			uint8_t r = *pixels++;
			*dst++ = r;
			*dst++ = g;
			*dst++ = b;
			*dst++ = 0xff;
		}
	} else if (pixels_size == 0) {
		const uint8_t *alpha = pixels;
		for (uint32_t i = 0; i < width * height; i++) {
			dst[3] = *alpha++;
			dst += 4;
		}
	} else {
		const uint8_t *alpha = pixels + pixels_size;
		for (uint32_t i = 0; i < width * height; i++) {
			uint8_t b = *pixels++;
			uint8_t g = *pixels++;
			uint8_t r = *pixels++;
			*dst++ = r;
			*dst++ = g;
			*dst++ = b;
			*dst++ = *alpha++;
		}
	}
}
