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

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <turbojpeg.h>
#include <webp/decode.h>
#include <zlib.h>
#include "little_endian.h"
#include "system4.h"
#include "system4/cg.h"
#include "system4/pms.h"
#include "system4/webp.h"

bool ajp_checkfmt(const uint8_t *data)
{
	if (data[0] != 'A' || data[1] != 'J' || data[2] != 'P' || data[3] != '\0')
		return false;
	return true;
}

static const uint8_t ajp_key[] = {
	0x5d, 0x91, 0xae, 0x87,
	0x4a, 0x56, 0x41, 0xcd,
	0x83, 0xec, 0x4c, 0x92,
	0xb5, 0xcb, 0x16, 0x34
};

static void ajp_decrypt(uint8_t *data, size_t size)
{
	for (size_t i = 0; i < 16 && i < size; i++) {
		data[i] ^= ajp_key[i];
	}
}

struct ajp_header {
	uint32_t width;
	uint32_t height;
	uint32_t jpeg_off;
	uint32_t jpeg_size;
	uint32_t mask_off;
	uint32_t mask_size;
};

static void ajp_extract_header(const uint8_t *data, struct ajp_header *ajp)
{
	ajp->width     = LittleEndian_getDW(data, 12);
	ajp->height    = LittleEndian_getDW(data, 16);
	ajp->jpeg_off  = LittleEndian_getDW(data, 20);
	ajp->jpeg_size = LittleEndian_getDW(data, 24);
	ajp->mask_off  = LittleEndian_getDW(data, 28);
	ajp->mask_size = LittleEndian_getDW(data, 32);
}

static void ajp_init_metrics(struct ajp_header *ajp, struct cg_metrics *dst)
{
	dst->w = ajp->width;
	dst->h = ajp->height;
	dst->bpp = 24;
	dst->has_pixel = ajp->jpeg_size > 0;
	dst->has_alpha = ajp->mask_size > 0;
	dst->pixel_pitch = ajp->width * 3;
	dst->alpha_pitch = 1;
}

static uint8_t *read_mask(uint8_t *pixels, uint8_t *mask_data, struct ajp_header *ajp)
{
	if (ajp->mask_size && pms8_checkfmt(mask_data)) {
		return pms_extract_mask(mask_data, ajp->mask_size);
	} else if (ajp->mask_size && webp_checkfmt(mask_data)) {
		int w, h;
		uint8_t *tmp = WebPDecodeRGBA(mask_data, ajp->mask_size, &w, &h);
		if (w != ajp->width || h != ajp->height) {
			WARNING("Unexpected AJP mask size");
			WebPFree(tmp);
			return NULL;
		}
		// discard pixel data
		uint8_t *mask = xmalloc(ajp->width * ajp->height);
		for (int i = 0; i < ajp->width * ajp->height; i++) {
			mask[i] = tmp[i*4+3];
		}
		WebPFree(tmp);
		return mask;
	} else if (mask_data[0] == 0x78) {
		// compressed
		unsigned long uncompressed_size = ajp->width * ajp->height;
		uint8_t *mask = xmalloc(uncompressed_size);
		if (uncompress(mask, &uncompressed_size, mask_data, ajp->mask_size) != Z_OK) {
			WARNING("uncompress failed");
			free(mask);
			return NULL;
		} else if (uncompressed_size != (unsigned)ajp->width * (unsigned)ajp->height) {
			WARNING("Unexpected AJP mask size");
		}
		return mask;
	}
	if (ajp->mask_size)
		WARNING("Unsupported AJP mask format: %02x %02x %02x %02x",
				mask_data[0], mask_data[1], mask_data[2], mask_data[3]);

	return NULL;
}

static uint8_t *load_mask(uint8_t *pixels, uint8_t *mask_data, struct ajp_header *ajp)
{
	uint8_t *mask = read_mask(pixels, mask_data, ajp);
	if (!mask) {
		mask = xmalloc(ajp->width * ajp->height);
		memset(mask, 0xFF, ajp->width * ajp->height);
	}

	uint8_t *out = xmalloc(ajp->width * ajp->height * 4);
	for (int i = 0; i < ajp->width * ajp->height; i++) {
		out[i*4+0] = pixels[i*3+0];
		out[i*4+1] = pixels[i*3+1];
		out[i*4+2] = pixels[i*3+2];
		out[i*4+3] = mask[i];
	}
	free(pixels);
	free(mask);
	return out;
}

void ajp_extract(const uint8_t *data, size_t size, struct cg *cg)
{
	uint8_t *buf = NULL, *jpeg_data = NULL, *mask_data = NULL;
	int width, height, subsamp;
	struct ajp_header ajp;
	ajp_extract_header(data, &ajp);
	ajp_init_metrics(&ajp, &cg->metrics);

	if (ajp.jpeg_off > size) {
		WARNING("AJP JPEG offset invalid");
		return;
	}
	if (ajp.jpeg_off + ajp.jpeg_size > size) {
		WARNING("AJP JPEG size invalid");
		return;
	}
	if (ajp.mask_off > size) {
		WARNING("AJP mask offset invalid");
		return;
	}
	if (ajp.mask_off + ajp.mask_size > size) {
		WARNING("AJP mask size invalid");
		return;
	}

	jpeg_data = xmalloc(ajp.jpeg_size);
	mask_data = xmalloc(ajp.mask_size);
	memcpy(jpeg_data, data + ajp.jpeg_off, ajp.jpeg_size);
	memcpy(mask_data, data + ajp.mask_off, ajp.mask_size);
	ajp_decrypt(jpeg_data, ajp.jpeg_size);
	ajp_decrypt(mask_data, ajp.mask_size);

	tjhandle decompressor = tjInitDecompress();
	tjDecompressHeader2(decompressor, jpeg_data, ajp.jpeg_size, &width, &height, &subsamp);
	if ((uint32_t)width != ajp.width)
		WARNING("AJP width doesn't match JPEG width (%d vs. %u)", width, ajp.width);
	if ((uint32_t)height != ajp.height)
		WARNING("AJP height doesn't match JPEG height (%d vs. %u)", height, ajp.height);

	buf = xmalloc(width * height * 3);
	if (tjDecompress2(decompressor, jpeg_data, ajp.jpeg_size, buf, width, 0, height, TJPF_RGB, TJFLAG_FASTDCT) < 0) {
		WARNING("JPEG decompression failed: %s", tjGetErrorStr());
		free(buf);
		goto cleanup;
	}

	buf = load_mask(buf, mask_data, &ajp);

	cg->type = ALCG_AJP;
	cg->pixels = buf;

cleanup:
	free(jpeg_data);
	free(mask_data);
	tjDestroy(decompressor);
}
