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
#include <assert.h>
#include <png.h>

#include "system4.h"
#include "system4/ald.h"
#include "system4/buffer.h"
#include "system4/cg.h"
#include "system4/png.h"

#include "little_endian.h"

bool png_cg_checkfmt(const uint8_t *data)
{
	return data && png_check_sig((png_const_bytep)data, 8);
}

static void read_png_data(png_structp png_ptr, png_bytep out, size_t length)
{
	if (png_ptr == NULL)
		return;

	struct buffer *buf = (struct buffer*) png_get_io_ptr(png_ptr);
	if (buffer_remaining(buf) < length) {
		png_error(png_ptr, "png truncated");
		return;
	}

	buffer_read_bytes(buf, out, length);
}

struct png_reader {
	png_structp png_ptr;
	png_infop info_ptr;
	png_uint_32 w;
	png_uint_32 h;
	int bit_depth;
	int color_type;
	struct buffer buf;
};

static bool png_init_structs(struct png_reader *r, const uint8_t *data, size_t size)
{
	memset(r, 0, sizeof(*r));
	if (!data || size < 8 || !png_check_sig((png_const_bytep)data, 8)) {
		WARNING("Invalid PNG signature");
		return false;
	}

	buffer_init(&r->buf, (uint8_t*)data, size);
	r->png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	if (!r->png_ptr) {
		WARNING("png_create_read_struct failed");
		return false;
	}

	r->info_ptr = png_create_info_struct(r->png_ptr);
	if (!r->info_ptr) {
		WARNING("png_create_info_struct failed");
		png_destroy_read_struct(&r->png_ptr, NULL, NULL);
		return false;
	}
	return true;
}

static bool png_populate_metrics(struct png_reader *r, struct cg_metrics *metrics)
{
	if (r->w == 0 || r->h == 0 || r->w > 32768 || r->h > 32768) {
		WARNING("Invalid PNG dimensions: %ux%u", (unsigned)r->w, (unsigned)r->h);
		return false;
	}

	bool has_trns = png_get_valid(r->png_ptr, r->info_ptr, PNG_INFO_tRNS) != 0;
	metrics->x = 0;
	metrics->y = 0;
	metrics->w = (int)r->w;
	metrics->h = (int)r->h;
	metrics->bpp = 24;
	metrics->has_pixel = true;
	metrics->has_alpha = (r->color_type & PNG_COLOR_MASK_ALPHA) != 0 || has_trns;
	metrics->pixel_pitch = 3;
	metrics->alpha_pitch = 1;
	return true;
}

bool png_cg_get_metrics(const uint8_t *data, size_t size, struct cg_metrics *dst)
{
	struct png_reader r;
	if (!png_init_structs(&r, data, size))
		return false;

	if (setjmp(png_jmpbuf(r.png_ptr))) {
		WARNING("png_cg_get_metrics failed");
		png_destroy_read_struct(&r.png_ptr, &r.info_ptr, NULL);
		return false;
	}

	buffer_skip(&r.buf, 8);
	png_set_read_fn(r.png_ptr, &r.buf, read_png_data);
	png_set_sig_bytes(r.png_ptr, 8);
	png_read_info(r.png_ptr, r.info_ptr);

	if (png_get_IHDR(r.png_ptr, r.info_ptr, &r.w, &r.h, &r.bit_depth, &r.color_type, NULL, NULL, NULL) != 1 ||
	    !png_populate_metrics(&r, dst)) {
		png_destroy_read_struct(&r.png_ptr, &r.info_ptr, NULL);
		return false;
	}

	png_destroy_read_struct(&r.png_ptr, &r.info_ptr, NULL);
	return true;
}

void png_cg_extract(const uint8_t *data, size_t size, struct cg *cg)
{
	struct png_reader r;
	uint8_t * volatile pixels = NULL;

	if (!png_init_structs(&r, data, size))
		return;

	if (setjmp(png_jmpbuf(r.png_ptr))) {
		WARNING("png_cg_extract failed");
		free(pixels);
		cg->pixels = NULL;
		png_destroy_read_struct(&r.png_ptr, &r.info_ptr, NULL);
		return;
	}

	buffer_skip(&r.buf, 8);
	png_set_read_fn(r.png_ptr, &r.buf, read_png_data);
	png_set_sig_bytes(r.png_ptr, 8);
	png_read_info(r.png_ptr, r.info_ptr);

	if (png_get_IHDR(r.png_ptr, r.info_ptr, &r.w, &r.h, &r.bit_depth, &r.color_type, NULL, NULL, NULL) != 1 ||
	    !png_populate_metrics(&r, &cg->metrics)) {
		png_destroy_read_struct(&r.png_ptr, &r.info_ptr, NULL);
		return;
	}

	if (r.color_type == PNG_COLOR_TYPE_PALETTE)
		png_set_palette_to_rgb(r.png_ptr);
	if (r.color_type == PNG_COLOR_TYPE_GRAY && r.bit_depth < 8)
		png_set_expand_gray_1_2_4_to_8(r.png_ptr);
	if (png_get_valid(r.png_ptr, r.info_ptr, PNG_INFO_tRNS))
		png_set_tRNS_to_alpha(r.png_ptr);
	if (r.bit_depth == 16)
		png_set_scale_16(r.png_ptr);
	if (r.bit_depth < 8)
		png_set_packing(r.png_ptr);
	if (r.color_type == PNG_COLOR_TYPE_GRAY || r.color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
		png_set_gray_to_rgb(r.png_ptr);
	png_set_add_alpha(r.png_ptr, 0xFF, PNG_FILLER_AFTER);
	int nr_passes = png_set_interlace_handling(r.png_ptr);

	png_read_update_info(r.png_ptr, r.info_ptr);

	int out_color_type = png_get_color_type(r.png_ptr, r.info_ptr);
	size_t w = (size_t)cg->metrics.w;
	size_t h = (size_t)cg->metrics.h;
	size_t row_bytes = png_get_rowbytes(r.png_ptr, r.info_ptr);
	if (out_color_type != PNG_COLOR_TYPE_RGB_ALPHA || row_bytes != w * 4) {
		WARNING("Unsupported PNG row layout (color_type=%d, row_bytes=%zu, w=%zu)", out_color_type, row_bytes, w);
		png_destroy_read_struct(&r.png_ptr, &r.info_ptr, NULL);
		return;
	}

	pixels = xmalloc(w * h * 4);
	for (int pass = 0; pass < nr_passes; pass++) {
		for (size_t row = 0; row < h; row++) {
			png_read_row(r.png_ptr, (png_bytep)(pixels + row * w * 4), NULL);
		}
	}

	cg->type = ALCG_PNG;
	cg->pixels = pixels;
	png_destroy_read_struct(&r.png_ptr, &r.info_ptr, NULL);
}

int png_cg_write(struct cg *cg, FILE *f)
{
	int r = 0;
	png_structp png_ptr = NULL;
	png_infop info_ptr = NULL;
	png_byte **row_pointers = NULL;

	png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	if (!png_ptr) {
		WARNING("png_create_write_struct failed");
		goto cleanup;
	}

	info_ptr = png_create_info_struct(png_ptr);
	if (!info_ptr) {
		WARNING("png_create_info_struct failed");
		goto cleanup;
	}

	if (setjmp(png_jmpbuf(png_ptr))) {
		WARNING("png_init_io failed");
		goto cleanup;
	}

	png_init_io(png_ptr, f);

	if (setjmp(png_jmpbuf(png_ptr))) {
		WARNING("png_write_header failed");
		goto cleanup;
	}

	png_set_IHDR(png_ptr, info_ptr, cg->metrics.w, cg->metrics.h,
		     8, PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE,
		     PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);
	png_write_info(png_ptr, info_ptr);

	png_uint_32 stride = cg->metrics.w * 4;
	row_pointers = png_malloc(png_ptr, cg->metrics.h * sizeof(png_byte*));
	for (int i = 0; i < cg->metrics.h; i++) {
		row_pointers[i] = cg->pixels + i*stride;
	}

	if (setjmp(png_jmpbuf(png_ptr))) {
		WARNING("png_write_image failed");
		goto cleanup;
	}

	png_write_image(png_ptr, row_pointers);

	if (setjmp(png_jmpbuf(png_ptr))) {
		WARNING("png_write_end failed");
		goto cleanup;
	}

	png_write_end(png_ptr, NULL);
	r = 1;
cleanup:
	if (row_pointers)
		png_free(png_ptr, row_pointers);
	if (png_ptr)
		png_destroy_write_struct(&png_ptr, info_ptr ? &info_ptr : NULL);
	return r;
}
