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
#include <png.h>

#include "system4.h"
#include "system4/ald.h"
#include "system4/cg.h"
#include "system4/png.h"

#include "little_endian.h"

bool png_cg_checkfmt(const uint8_t *data)
{
	if (data[0] != 137 || data[1] != 80 || data[2] != 78 || data[3] != 71)
		return false;
	if (data[4] != 13 || data[5] != 10 || data[6] != 26 || data[7] != 10)
		return false;
	return true;
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

