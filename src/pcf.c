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
#include <stdint.h>
#include <string.h>
#include "little_endian.h"
#include "system4.h"
#include "system4/buffer.h"
#include "system4/cg.h"
#include "system4/pcf.h"
#include "system4/qnt.h"
#include "system4/string.h"

bool pcf_checkfmt(const uint8_t *data)
{
	return data[0] == 'p' && data[1] == 'c' && data[2] == 'f' && data[3] == ' ';
}

struct pcf_header {
	// pcf section
	int version;
	int width;
	int height;
	int bpp;
	struct string *str;
	// ptdl section
	int x;
	int y;
	int unknown2;
	int unknown3;
};

static void pcf_header_free(struct pcf_header *hdr)
{
	if (hdr->str)
		free_string(hdr->str);
}

static bool pcf_read_pcf(struct buffer *in, struct pcf_header *out)
{
	if (!buffer_check_bytes(in, "pcf ", 4)) {
		WARNING("Not a pcf File");
		return false;
	}
	int32_t pcf_size = buffer_read_int32(in);
	size_t start = in->index;

	if ((out->version = buffer_read_int32(in)) != 1) {
		WARNING("Unsupported pcf version");
		return false;
	}

	out->width = buffer_read_int32(in);
	out->height = buffer_read_int32(in);
	out->bpp = buffer_read_int32(in);
	out->str = buffer_read_pascal_string(in);

	if (in->index != start + pcf_size) {
		WARNING("pcf header size didn't match");
		in->index = start + pcf_size;
	}
	return true;
}

static bool pcf_read_ptdl(struct buffer *in, struct pcf_header *out)
{
	if (!buffer_check_bytes(in, "ptdl", 4)) {
		WARNING("Unexpected data at ptdl header");
		return false;
	}
	int32_t ptdl_size = buffer_read_int32(in);
	size_t start = in->index;

	out->x = buffer_read_int32(in);
	out->y = buffer_read_int32(in);
	out->unknown2 = buffer_read_int32(in);
	out->unknown3 = buffer_read_int32(in);

	if (in->index != start + ptdl_size) {
		WARNING("ptdl header size didn't match");
		in->index = start + ptdl_size;
	}
	return true;
}

static struct cg *pcf_read_pcgd(struct buffer *in, struct pcf_header *hdr)
{
	if (!buffer_check_bytes(in, "pcgd", 4)) {
		WARNING("Unexpected data at pcgd header");
		return NULL;
	}
	int32_t pcgd_size = buffer_read_int32(in);
	if (pcgd_size < 4 || strncmp(buffer_strdata(in), "QNT", 4)) {
		WARNING("pcf CG isn't qnt format");
		return NULL;
	}

	return cg_load_buffer((uint8_t*)buffer_strdata(in), buffer_remaining(in));
}

static void pcf_init_metrics(struct pcf_header *pcf, struct cg_metrics *dst)
{
	dst->w = pcf->width;
	dst->h = pcf->height;
	dst->bpp = pcf->bpp;
	dst->has_pixel = true;
	dst->has_alpha = true;
	dst->pixel_pitch = pcf->width * (pcf->bpp / 8);
	dst->alpha_pitch = 1;
}

bool pcf_extract(const uint8_t *data, size_t size, struct cg *cg)
{
	struct pcf_header hdr = {0};
	struct buffer in;
	buffer_init(&in, (uint8_t*)data, size);
	if (!pcf_read_pcf(&in, &hdr)) {
		goto error;
	}
	if (!pcf_read_ptdl(&in, &hdr)) {
		goto error;
	}
	struct cg *cg_data = pcf_read_pcgd(&in, &hdr);
	if (!cg_data) {
		goto error;
	}

	uint32_t *pixels = xcalloc(hdr.width * hdr.height, 4);
	for (int row = 0; row < cg_data->metrics.h; row++) {
		for (int col = 0; col < cg_data->metrics.w; col++) {
			const int dst_i = (hdr.y + row) * hdr.width + (hdr.x + col);
			const int src_i = row * cg_data->metrics.w + col;
			pixels[dst_i] = ((uint32_t*)cg_data->pixels)[src_i];
		}
	}
	cg->pixels = pixels;
	pcf_init_metrics(&hdr, &cg->metrics);

	cg_free(cg_data);
	pcf_header_free(&hdr);
	return true;
error:
	pcf_header_free(&hdr);
	return false;
}

bool pcf_get_metrics(const uint8_t *data, size_t size, struct cg_metrics *dst)
{
	struct pcf_header hdr = {0};
	struct buffer in;
	buffer_init(&in, (uint8_t*)data, size);
	if (!pcf_read_pcf(&in, &hdr))
		return false;
	pcf_init_metrics(&hdr, dst);
	return true;
}
