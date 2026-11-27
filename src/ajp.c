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
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <pthread.h>
#endif
#include <turbojpeg.h>
#include <webp/decode.h>
#include "little_endian.h"
#include "system4.h"
#include "system4/cg.h"
#include "system4/pms.h"
#include "system4/webp.h"
#include "system4/zlib.h"

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
	dst->x = dst->y = 0;
	dst->w = ajp->width;
	dst->h = ajp->height;
	dst->bpp = 24;
	dst->has_pixel = ajp->jpeg_size > 0;
	dst->has_alpha = ajp->mask_size > 0;
	dst->pixel_pitch = ajp->width * 3;
	dst->alpha_pitch = 1;
}

bool ajp_get_metrics(const uint8_t *data, size_t size, struct cg_metrics *dst)
{
	if (!data || size < 36 || !ajp_checkfmt(data)) return false;
	struct ajp_header header;
	ajp_extract_header(data, &header);
	if (!header.width || !header.height || header.width > 32768 || header.height > 32768)
		return false;
	ajp_init_metrics(&header, dst);
	return true;
}

#ifndef _WIN32
#define AJP_TLS_SCRATCH_MAX (512u * 1024u)

struct ajp_tls {
	tjhandle tj;
	uint8_t *scratch;
	size_t scratch_cap;
};

static pthread_key_t ajp_tls_key;
static pthread_once_t ajp_tls_once = PTHREAD_ONCE_INIT;
static bool ajp_tls_key_ready = false;

static void ajp_tls_destructor(void *ptr)
{
	struct ajp_tls *tls = (struct ajp_tls *)ptr;
	if (!tls)
		return;
	if (tls->tj)
		tjDestroy(tls->tj);
	free(tls->scratch);
	free(tls);
}

static void ajp_tls_key_init(void)
{
	if (pthread_key_create(&ajp_tls_key, ajp_tls_destructor) == 0)
		ajp_tls_key_ready = true;
}

static struct ajp_tls *ajp_get_tls(void)
{
	pthread_once(&ajp_tls_once, ajp_tls_key_init);
	if (!ajp_tls_key_ready)
		return NULL;
	struct ajp_tls *tls = (struct ajp_tls *)pthread_getspecific(ajp_tls_key);
	if (!tls) {
		tls = calloc(1, sizeof(*tls));
		if (!tls)
			return NULL;
		if (pthread_setspecific(ajp_tls_key, tls) != 0) {
			free(tls);
			return NULL;
		}
	}
	return tls;
}

static tjhandle ajp_acquire_decompressor(struct ajp_tls *tls, bool *owned_out)
{
	if (tls) {
		if (!tls->tj)
			tls->tj = tjInitDecompress();
		*owned_out = false;
		return tls->tj;
	}
	*owned_out = true;
	return tjInitDecompress();
}

static void ajp_reset_decompressor(struct ajp_tls *tls, tjhandle h, bool owned)
{
	if (!h)
		return;
	if (!owned && tls && tls->tj == h)
		tls->tj = NULL;
	tjDestroy(h);
}

static uint8_t *ajp_acquire_scratch(struct ajp_tls *tls, size_t size, bool *heap_out)
{
	if (tls && size <= AJP_TLS_SCRATCH_MAX) {
		if (size > tls->scratch_cap) {
			uint8_t *grown = realloc(tls->scratch, size);
			if (!grown) {
				*heap_out = true;
				return xmalloc(size);
			}
			tls->scratch = grown;
			tls->scratch_cap = size;
		}
		*heap_out = false;
		return tls->scratch;
	}
	*heap_out = true;
	return xmalloc(size);
}
#else
struct ajp_tls {
	int unused;
};

static struct ajp_tls *ajp_get_tls(void)
{
	return NULL;
}

static tjhandle ajp_acquire_decompressor(struct ajp_tls *tls, bool *owned_out)
{
	(void)tls;
	*owned_out = true;
	return tjInitDecompress();
}

static void ajp_reset_decompressor(struct ajp_tls *tls, tjhandle h, bool owned)
{
	(void)tls;
	(void)owned;
	if (h)
		tjDestroy(h);
}

static uint8_t *ajp_acquire_scratch(struct ajp_tls *tls, size_t size, bool *heap_out)
{
	(void)tls;
	*heap_out = true;
	return xmalloc(size);
}
#endif

static void apply_mask(uint8_t *rgba, const uint8_t *mask_data, const struct ajp_header *ajp)
{
	if (ajp->mask_size == 0)
		return;
	size_t npix = (size_t)ajp->width * (size_t)ajp->height;
	if (ajp->mask_size >= 44 && pms8_checkfmt_sized(mask_data, ajp->mask_size)) {
		struct cg_metrics mask_metrics = {0};
		if (!pms_get_metrics(mask_data, &mask_metrics) ||
		    mask_metrics.w <= 0 || mask_metrics.h <= 0 ||
		    (uint64_t)mask_metrics.w * (uint64_t)mask_metrics.h != (uint64_t)npix) {
			WARNING("Unexpected AJP PMS8 mask size (%dx%d vs. %ux%u)",
				mask_metrics.w, mask_metrics.h, ajp->width, ajp->height);
			return;
		}
		uint8_t *mask = pms_extract_mask(mask_data, ajp->mask_size);
		if (!mask)
			return;
		for (size_t i = 0; i < npix; i++) {
			rgba[i * 4 + 3] = mask[i];
		}
		free(mask);
	} else if (ajp->mask_size >= 12 && webp_checkfmt(mask_data)) {
		int w = 0, h = 0;
		uint8_t *tmp = WebPDecodeRGBA(mask_data, ajp->mask_size, &w, &h);
		if (!tmp || w <= 0 || h <= 0 || (uint32_t)w != ajp->width || (uint32_t)h != ajp->height) {
			WARNING("Unexpected AJP mask size");
			WebPFree(tmp);
			return;
		}
		for (size_t i = 0; i < npix; i++) {
			rgba[i * 4 + 3] = tmp[i * 4 + 3];
		}
		WebPFree(tmp);
	} else if (mask_data[0] == 0x78) {
		// compressed
		uint8_t *mask = xmalloc(npix);
		if (!zlib_decompress_exact(mask, npix, mask_data, ajp->mask_size)) {
			WARNING("uncompress failed");
			free(mask);
			return;
		}
		for (size_t i = 0; i < npix; i++) {
			rgba[i * 4 + 3] = mask[i];
		}
		free(mask);
	} else if (ajp->mask_size >= 4) {
		WARNING("Unsupported AJP mask format: %02x %02x %02x %02x",
				mask_data[0], mask_data[1], mask_data[2], mask_data[3]);
	}
}

void ajp_extract(const uint8_t *data, size_t size, struct cg *cg)
{
	uint8_t *buf = NULL, *jpeg_data = NULL, *mask_data = NULL;
	bool jpeg_heap = false, tj_owned = false;
	int width = 0, height = 0, subsamp = 0;
	struct ajp_header ajp;
	if (!data || size < 36 || !ajp_checkfmt(data))
		return;
	ajp_extract_header(data, &ajp);
	if (!ajp.width || !ajp.height || ajp.width > 32768 || ajp.height > 32768) {
		WARNING("Invalid AJP dimensions: %ux%u", ajp.width, ajp.height);
		return;
	}

	if (ajp.jpeg_off > size || ajp.jpeg_size > size - ajp.jpeg_off || ajp.jpeg_size < 2) {
		WARNING("AJP JPEG range invalid");
		return;
	}
	if (ajp.mask_off > size || ajp.mask_size > size - ajp.mask_off) {
		WARNING("AJP mask range invalid");
		return;
	}

	ajp_init_metrics(&ajp, &cg->metrics);

	struct ajp_tls *tls = ajp_get_tls();
	tjhandle decompressor = ajp_acquire_decompressor(tls, &tj_owned);
	if (!decompressor)
		return;

	jpeg_data = ajp_acquire_scratch(tls, ajp.jpeg_size, &jpeg_heap);
	memcpy(jpeg_data, data + ajp.jpeg_off, ajp.jpeg_size);
	ajp_decrypt(jpeg_data, ajp.jpeg_size);

	int hdr_rc = tjDecompressHeader2(decompressor, jpeg_data, ajp.jpeg_size, &width, &height, &subsamp);
	if (hdr_rc < 0 ||
	    width <= 0 || height <= 0 || width > 32768 || height > 32768 ||
	    (uint32_t)width != ajp.width || (uint32_t)height != ajp.height) {
		WARNING("AJP JPEG header invalid or dimension mismatch (%dx%d vs. %ux%u)",
			width, height, ajp.width, ajp.height);
		if (hdr_rc < 0) {
			ajp_reset_decompressor(tls, decompressor, tj_owned);
			decompressor = NULL;
		}
		goto cleanup;
	}

	buf = xmalloc((size_t)width * (size_t)height * 4);
	if (tjDecompress2(decompressor, jpeg_data, ajp.jpeg_size, buf, width, 0, height, TJPF_RGBA, TJFLAG_FASTDCT) < 0) {
		WARNING("JPEG decompression failed: %s", tjGetErrorStr2(decompressor));
		free(buf);
		ajp_reset_decompressor(tls, decompressor, tj_owned);
		decompressor = NULL;
		goto cleanup;
	}

	if (jpeg_heap) {
		free(jpeg_data);
		jpeg_data = NULL;
		jpeg_heap = false;
	}

	if (ajp.mask_size > 0) {
		bool mask_heap = false;
		mask_data = ajp_acquire_scratch(tls, ajp.mask_size, &mask_heap);
		memcpy(mask_data, data + ajp.mask_off, ajp.mask_size);
		ajp_decrypt(mask_data, ajp.mask_size);
		apply_mask(buf, mask_data, &ajp);
		if (mask_heap)
			free(mask_data);
	}

	cg->type = ALCG_AJP;
	cg->pixels = buf;

cleanup:
	if (jpeg_heap)
		free(jpeg_data);
	if (tj_owned && decompressor)
		tjDestroy(decompressor);
}
