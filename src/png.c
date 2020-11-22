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
	return png_check_sig(data, 8);
}

static void read_png_data(png_structp png_ptr, png_bytep out, size_t length)
{
	if (png_ptr == NULL)
		return;

	struct buffer *buf = (struct buffer*) png_get_io_ptr(png_ptr);
	if (buffer_remaining(buf) < length)
		ERROR("png truncated"); // FIXME: make this recoverable

	buffer_read_bytes(buf, out, length);
}

static void extract_rgb(png_structp png_ptr, png_infop info_ptr, struct cg *cg)
{
	const png_uint_32 row_bytes = png_get_rowbytes(png_ptr, info_ptr);
	uint8_t *row_data = xmalloc(row_bytes);
	uint8_t *pixels = cg->pixels;

	for (int row = 0; row < cg->metrics.h; row++) {
		png_read_row(png_ptr, (png_bytep)row_data, NULL);
		int row_off = row * cg->metrics.w * 4;
		int i = 0;
		for (int col = 0; col < cg->metrics.w; col++) {
			pixels[row_off + col*4 + 0] = row_data[i++];
			pixels[row_off + col*4 + 1] = row_data[i++];
			pixels[row_off + col*4 + 2] = row_data[i++];
			pixels[row_off + col*4 + 3] = 0xFF;
		}
	}

	free(row_data);
}

static void extract_rgba(png_structp png_ptr, png_infop info_ptr, struct cg *cg)
{
	assert((int)png_get_rowbytes(png_ptr, info_ptr) == cg->metrics.w*4);

	for (int row = 0; row < cg->metrics.h; row++) {
		uint8_t *row_data = (uint8_t*)cg->pixels + row*cg->metrics.w*4;
		png_read_row(png_ptr, (png_bytep)row_data, NULL);
	}
}

static int png_read_init(png_structp *png_ptr_out, png_infop *info_ptr_out, struct cg_metrics *metrics, struct buffer *buf)
{
	png_structp png_ptr = NULL;
	png_infop info_ptr = NULL;

	png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	if (!png_ptr) {
		WARNING("png_create_read_struct failed");
		goto fail;
	}

	info_ptr = png_create_info_struct(png_ptr);
	if (!info_ptr) {
		WARNING("png_create_info_struct failed");
		goto fail;
	}

	if (!png_check_sig((uint8_t*)buffer_strdata(buf), 8)) {
		WARNING("Invalid PNG signature");
		goto fail;
	}

	buffer_skip(buf, 8);
	png_set_read_fn(png_ptr, buf, read_png_data);
	png_set_sig_bytes(png_ptr, 8);

	png_read_info(png_ptr, info_ptr);
	int bit_depth = 0;
	int color_type = -1;
	png_uint_32 r = png_get_IHDR(png_ptr, info_ptr,
				     (png_uint_32*)&metrics->w,
				     (png_uint_32*)&metrics->h,
				     &bit_depth,
				     &color_type,
				     NULL, NULL, NULL);
	if (r != 1) {
		WARNING("png_get_IHDR failed");
		goto fail;
	}

	if (color_type != PNG_COLOR_TYPE_RGB && color_type != PNG_COLOR_TYPE_RGB_ALPHA) {
		WARNING("Unsupported PNG color type");
		goto fail;
	}

	metrics->bpp = 24;
	metrics->has_pixel = true;
	metrics->has_alpha = color_type == PNG_COLOR_TYPE_RGB_ALPHA;
	metrics->pixel_pitch = 3;
	metrics->alpha_pitch = 1;

	*png_ptr_out = png_ptr;
	*info_ptr_out = info_ptr;
	return 1;
fail:
	if (png_ptr)
		png_destroy_read_struct(&png_ptr, info_ptr ? &info_ptr : NULL, NULL);
	return 0;

}

bool png_cg_get_metrics(const uint8_t *data, size_t size, struct cg_metrics *dst)
{
	struct buffer buf;
	png_structp png_ptr = NULL;
	png_infop info_ptr = NULL;

	buffer_init(&buf, (uint8_t*)data, size);
	if (!png_read_init(&png_ptr, &info_ptr, dst, &buf))
		return false;

	png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
	return true;
}

void png_cg_extract(const uint8_t *data, size_t size, struct cg *cg)
{
	struct buffer buf;
	png_structp png_ptr = NULL;
	png_infop info_ptr = NULL;

	buffer_init(&buf, (uint8_t*)data, size);
	if (!png_read_init(&png_ptr, &info_ptr, &cg->metrics, &buf))
		return;

	cg->pixels = xmalloc(cg->metrics.w * cg->metrics.h * 4);
	if (cg->metrics.has_alpha) {
		extract_rgba(png_ptr, info_ptr, cg);
	} else {
		extract_rgb(png_ptr, info_ptr, cg);
	}

	png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
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

