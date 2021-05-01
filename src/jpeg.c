/* Copyright (C) 2021 kichikuou <KichikuouChrome@gmail.com>
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
#include <turbojpeg.h>
#include "system4.h"
#include "system4/cg.h"
#include "system4/jpeg.h"

bool jpeg_cg_checkfmt(const uint8_t *data)
{
	return data[0] == 0xff && data[1] == 0xd8;
}

static bool get_metrics(tjhandle decompressor, const uint8_t *data, size_t size, struct cg_metrics *dst)
{
	int width, height, subsamp;
	if (tjDecompressHeader2(decompressor, (unsigned char *)data, size, &width, &height, &subsamp) < 0) {
		WARNING("tjDecompressHeader2 failed: %s", tjGetErrorStr());
		return false;
	}
	dst->w = width;
	dst->h = height;
	dst->bpp = 24;
	dst->has_pixel = true;
	dst->has_alpha = false;
	dst->pixel_pitch = width * 3;
	dst->alpha_pitch = 1;
	return true;
}

bool jpeg_cg_get_metrics(const uint8_t *data, size_t size, struct cg_metrics *dst)
{
	tjhandle decompressor = tjInitDecompress();
	bool result = get_metrics(decompressor, data, size, dst);
	tjDestroy(decompressor);
	return result;
}

void jpeg_cg_extract(const uint8_t *data, size_t size, struct cg *cg)
{
	tjhandle decompressor = tjInitDecompress();

	if (!get_metrics(decompressor, data, size, &cg->metrics))
		goto cleanup;

	uint8_t *buf = xmalloc(cg->metrics.w * cg->metrics.h * 4);
	if (tjDecompress2(decompressor, data, size, buf, cg->metrics.w, 0, cg->metrics.h, TJPF_RGBA, 0) < 0) {
		WARNING("JPEG decompression failed: %s", tjGetErrorStr());
		free(buf);
		goto cleanup;
	}
	cg->type = ALCG_JPEG;
	cg->pixels = buf;

cleanup:
	tjDestroy(decompressor);
}
