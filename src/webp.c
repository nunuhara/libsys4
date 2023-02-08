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

#include <stdlib.h>
#include <string.h>
#include <webp/decode.h>

#include "system4.h"
#include "system4/ald.h"
#include "system4/cg.h"
#include "system4/file.h"
#include "system4/webp.h"

#include "little_endian.h"

bool webp_checkfmt(const uint8_t *data)
{
	if (data[0] != 'R' || data[1] != 'I' || data[2] != 'F' || data[3] != 'F')
		return false;
	if (data[8] != 'W' || data[9] != 'E' || data[10] != 'B' || data[11] != 'P')
		return false;
	return true;
}

static void webp_init_metrics(struct cg_metrics *m)
{
	m->x = 0;
	m->y = 0;
	m->bpp = 24;
	m->has_pixel = true;
	m->has_alpha = true; // FIXME
	m->pixel_pitch = m->w * 3;
	m->alpha_pitch = 1;
}

void webp_get_metrics(uint8_t *data, size_t size, struct cg_metrics *m)
{
	WebPGetInfo(data, size, &m->w, &m->h);
	webp_init_metrics(m);
}

static int get_base_cg(uint8_t *data, size_t size)
{
	if (size >= 20 && data[size-12] == 'O' && data[size-11] == 'V'
			&& data[size-10] == 'E' && data[size-9] == 'R') {
		data += size - 12;
	}
	else if (size >= 32 && data[size-24] == 'O' && data[size-23] == 'V'
			&& data[size-22] == 'E' && data[size-21] == 'R') {
		data += size - 24;
	} else {
		return -1;
	}

	int uk = LittleEndian_getDW(data, 4); // size?
	if (uk != 4)
		WARNING("WEBP: expected 0x4 preceding base CG number, got %d", uk);
	return LittleEndian_getDW(data, 8);
}

void webp_extract(uint8_t *data, size_t size, struct cg *cg, struct archive *ar)
{
	cg->pixels = WebPDecodeRGBA(data, size, &cg->metrics.w, &cg->metrics.h);
	webp_init_metrics(&cg->metrics);
	cg->type = ALCG_WEBP;

	if (!ar)
		return;

	int base = get_base_cg(data, size);
	if (base < 0)
		return;

	// FIXME: possible infinite recursion on broken/malicious ALD
	struct cg *base_cg = cg_load(ar, base-1);
	if (!base_cg) {
		WARNING("failed to load webp base CG");
		return;
	}
	if (base_cg->metrics.w != cg->metrics.w || base_cg->metrics.h != cg->metrics.h) {
		WARNING("webp base CG dimensions don't match: (%d,%d) / (%d,%d)",
		        base_cg->metrics.w, base_cg->metrics.h, cg->metrics.w, cg->metrics.h);
		cg_free(base_cg);
		return;
	}

	// mask alpha color
	uint8_t *pixels = cg->pixels;
	uint8_t *base_pixels = base_cg->pixels;
	for (int row = 0; row < cg->metrics.h; row++) {
		for (int x = 0; x < cg->metrics.w; x++) {
			int p = cg->metrics.w*row*4 + x*4;
			if (pixels[p] == 255 && pixels[p+1] == 0 && pixels[p+2] == 255) {
				pixels[p+0] = base_pixels[p+0];
				pixels[p+1] = base_pixels[p+1];
				pixels[p+2] = base_pixels[p+2];
				pixels[p+3] = base_pixels[p+3];
			}
		}
	}

	cg_free(base_cg);
}

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <webp/encode.h>

int webp_write(struct cg *cg, FILE *f)
{
	uint8_t *out;
	size_t len = WebPEncodeLosslessRGBA(cg->pixels, cg->metrics.w, cg->metrics.h, cg->metrics.w*4, &out);
	if (fwrite(out, len, 1, f) != 1) {
		WARNING("webp_write: %s", strerror(errno));
		return 0;
	}
	WebPFree(out);
	return 1;
}

void webp_save(const char *path, uint8_t *pixels, int w, int h, bool alpha)
{
	size_t len;
	uint8_t *output;
	FILE *f = file_open_utf8(path, "wb");
	if (!f) {
		WARNING("fopen failed: %s", strerror(errno));
		return;
	}
	if (alpha) {
		len = WebPEncodeLosslessRGBA(pixels, w, h, w*4, &output);
	} else {
		len = WebPEncodeLosslessRGB(pixels, w, h, w*3, &output);
	}
	if (!fwrite(output, len, 1, f))
		WARNING("fwrite failed: %s", strerror(errno));
	fclose(f);
	WebPFree(output);
}
