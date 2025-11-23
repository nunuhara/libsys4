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
#include "assert.h"
#include "little_endian.h"
#include "system4.h"
#include "system4/archive.h"
#include "system4/buffer.h"
#include "system4/cg.h"
#include "system4/dcf.h"
#include "system4/qnt.h"
#include "system4/string.h"
#include "system4/utfsjis.h"
#include <zlib.h>

bool dcf_checkfmt(const uint8_t *data)
{
	return !strncmp((const char*)data, "dcf ", 4);
}

struct dcf_header {
	int width;
	int height;
	int bpp;
	char *base_cg_name;
};

static bool dcf_read_header(struct buffer *in, struct dcf_header *out)
{
	if (strncmp(buffer_strdata(in), "dcf ", 4)) {
		WARNING("Not a DCF File");
		return false;
	}

	buffer_skip(in, 4);
	int32_t header_size = buffer_read_int32(in);
	if (header_size < 0 || header_size > 4096) {
		WARNING("Invalid header size in DCF file");
		return false;
	}
	size_t next_pos = in->index + header_size;

	if (buffer_read_int32(in) != 1) {
		WARNING("Unsupported DCF version");
		return false;
	}

	out->width = buffer_read_int32(in);
	out->height = buffer_read_int32(in);
	out->bpp = buffer_read_int32(in);

	if (out->bpp != 32) {
		WARNING("Unsupported BPP in DCF file");
		return false;
	}

	int32_t name_length = buffer_read_int32(in);
	if (name_length < 0 || name_length > 2000) {
		WARNING("Invalid base CG name length in DCF header");
		return false;
	}

	uint8_t *name = xmalloc(name_length + 1);
	buffer_read_bytes(in, name, name_length);
	name[name_length] = '\0';

	uint8_t rot = (name_length % 7) + 1;
	for (int i = 0; i < name_length; i++) {
		name[i] = (name[i] << rot) | (name[i] >> (8-rot));
	}

	out->base_cg_name = (char*)name;
	if (in->index != next_pos) {
		WARNING("Extra data at end of DCF header");
		buffer_seek(in, next_pos);
	}

	return true;
}

static uint8_t *dcf_read_dfdl(struct buffer *in, size_t *size_out)
{
	if (strncmp(buffer_strdata(in), "dfdl", 4)) {
		WARNING("Expected dfdl section");
		return NULL;
	}

	buffer_skip(in, 4);
	int32_t dfdl_size = buffer_read_int32(in);
	if (dfdl_size < 0 || dfdl_size > 10000) {
		WARNING("Invalid size for dfdl section");
		return NULL;
	}
	size_t next_pos = in->index + dfdl_size;

	unsigned long uncompressed_size = buffer_read_int32(in);
	if (uncompressed_size > 40000) {
		WARNING("Invalid size for uncompressed chunk map");
		return NULL;
	}

	uint8_t *chunk_map = xmalloc(uncompressed_size);
	if (uncompress(chunk_map, &uncompressed_size, in->buf+in->index, dfdl_size - 4) != Z_OK) {
		WARNING("Failed to uncompress chunk map");
		free(chunk_map);
		return NULL;
	}

	buffer_seek(in, next_pos);
	*size_out = uncompressed_size;
	return chunk_map;
}

const uint8_t *dcf_read_dcgd(struct buffer *in, size_t *size_out)
{
	if (strncmp(buffer_strdata(in), "dcgd", 4)) {
		WARNING("Expected dcgd section");
		return NULL;
	}

	buffer_skip(in, 4);
	int32_t dcgd_size = buffer_read_int32(in);
	if (dcgd_size < 0 || dcgd_size > in->size - in->index) {
		WARNING("Invalid size for dcgd section");
		return NULL;
	}

	*size_out = dcgd_size;
	return in->buf + in->index;
}

void dcf_blit(struct cg *base, struct cg *diff, int x, int y, int w, int h)
{
	assert(x + w <= base->metrics.w);
	assert(y + h <= base->metrics.h);
	const int x_off = (x % base->metrics.w) * 4;
	const int stride = base->metrics.w * 4;
	uint8_t *base_px = base->pixels;
	uint8_t *diff_px = diff->pixels;

	for (int row = 0; row < h; row++) {
		int off = (stride * (row + y)) + x_off;
		memcpy(base_px + off, diff_px + off, w * 4);
	}
}

// FIXME: in xsystem4, this should be done in a shader
void dcf_apply_diff(struct cg *base, struct cg *diff, const uint8_t *chunk_map, size_t chunk_map_size)
{
	if (base->metrics.w != diff->metrics.w) {
		WARNING("DCF base CG width differs: %u / %u", base->metrics.w, diff->metrics.w);
		return;
	}

	if (base->metrics.h != diff->metrics.h) {
		WARNING("DCF base CG height differs");
		return;
	}

	const int chunks_w = base->metrics.w / 16;
	const int chunks_h = base->metrics.h / 16;
	for (size_t i = 0; i < chunk_map_size; i++) {
		if (chunk_map[i])
			continue;
		int chunk_x = i % chunks_w;
		int chunk_y = i / chunks_w;
		dcf_blit(base, diff, chunk_x * 16, chunk_y * 16, 16, 16);
	}

	// any leftover pixels that don't fit in a chunk are carried by the diff CG
	const int remaining_w = base->metrics.w % 16;
	const int remaining_h = base->metrics.h % 16;
	if (remaining_w) {
		dcf_blit(base, diff, chunks_w * 16, 0, remaining_w, base->metrics.h);
	}
	if (remaining_h) {
		dcf_blit(base, diff, 0, chunks_h * 16, base->metrics.w, remaining_h);
	}
}

static struct cg *dcf_get_base_cg(const char *name, struct archive *ar)
{
	struct string *tmp = (ar->conv ? ar->conv : make_string)(name, strlen(name));
	char *basename = archive_basename(tmp->text);
	free_string(tmp);

	struct archive_data *data = archive_get_by_basename(ar, basename);
	free(basename);
	if (!data)
		return NULL;
	struct cg *cg = cg_load_data(data);
	archive_free_data(data);
	return cg;
}

void dcf_extract(const uint8_t *data, size_t size, struct cg *cg, struct archive *ar)
{
	struct buffer buf;
	struct dcf_header hdr = {0};
	uint8_t *chunk_map = NULL;

	buffer_init(&buf, (uint8_t*)data, size);
	if (!dcf_read_header(&buf, &hdr)) {
		WARNING("Failed to read DCF header");
		goto cleanup;
	}

	size_t chunk_map_size;
	if (!(chunk_map = dcf_read_dfdl(&buf, &chunk_map_size))) {
		WARNING("Failed to read dfdl section of DCF file");
		goto cleanup;
	}

	if (LittleEndian_getDW(chunk_map, 0) != chunk_map_size - 4) {
		WARNING("Invalid size in chunk map");
		goto cleanup;
	}

	size_t cg_data_size;
	const uint8_t *cg_data = dcf_read_dcgd(&buf, &cg_data_size);
	if (!cg_data) {
		WARNING("Failed to read dcgd section of DCF file");
		goto cleanup;
	}

	if (!ar) {
		qnt_extract(cg_data, cg);
		goto cleanup;
	}

	struct cg *base_cg = dcf_get_base_cg(hdr.base_cg_name, ar);
	if (!base_cg) {
		char *u = sjis2utf(hdr.base_cg_name, strlen(hdr.base_cg_name));
		WARNING("Failed to load DCF base CG: \"%s\"", u);
		free(u);
		qnt_extract(cg_data, cg);
		goto cleanup;
	}

	*cg = *base_cg;
	free(base_cg);

	struct cg *diff_cg = cg_load_buffer((uint8_t*)cg_data, cg_data_size);
	if (!diff_cg) {
		WARNING("Failed to load DCF diff CG");
		goto cleanup;
	}

	dcf_apply_diff(cg, diff_cg, chunk_map + 4, chunk_map_size - 4);
	cg_free(diff_cg);

cleanup:
	free(chunk_map);
	free(hdr.base_cg_name);
}

static const uint8_t *dcf_get_qnt(const uint8_t *data)
{
	if (data[0] != 'd' && data[1] != 'c' && data[2] != 'f' && data[3] != 0x20)
		return NULL;

	int h2 = 8 + LittleEndian_getDW(data, 4);
	if (data[h2] != 'd' || data[h2+1] != 'f' || data[h2+2] != 'd' || data[h2+3] != 'l')
		return NULL;

	int h3 = h2 + 8 + LittleEndian_getDW(data, h2+4);
	if (data[h3] != 'd' || data[h3+1] != 'c' || data[h3+2] != 'g' || data[h3+3] != 'd')
		return NULL;

	if (data[h3+8] != 'Q' || data[h3+9] != 'N' || data[h3+10] != 'T' || data[h3+11] != '\0')
		return NULL;

	return data + h3 + 8;
}

void dcf_get_metrics(const uint8_t *data, possibly_unused size_t size, struct cg_metrics *m)
{
	if (!(data = dcf_get_qnt(data)))
		return;
	qnt_get_metrics(data, m);
}

char *dcf_get_base_cg_name(const uint8_t *data, size_t size)
{
	struct buffer buf;
	struct dcf_header hdr = {0};
	buffer_init(&buf, (uint8_t*)data, size);
	if (!dcf_read_header(&buf, &hdr)) {
		WARNING("Failed to read DCF header");
		return NULL;
	}

	return hdr.base_cg_name;
}

uint8_t *dcf_encode(struct cg *base, struct cg *diff, const char *base_cg_name, size_t *size_out)
{
	if ((base->metrics.w != diff->metrics.w) || (base->metrics.h != diff->metrics.h)) {
		WARNING("base and diff CG dimensions differ");
		return NULL;
	}

	// compute chunk map (1=identical chunks, 0=different)
	const int chunks_w = base->metrics.w / 16;
	const int chunks_h = base->metrics.h / 16;
	const int stride = base->metrics.w * 4;
	uint8_t *_chunk_map = xmalloc(4 + chunks_w * chunks_h);
	LittleEndian_putDW(_chunk_map, 0, chunks_w * chunks_h);
	uint8_t *chunk_map = _chunk_map + 4;
	memset(chunk_map, 1, chunks_w * chunks_h);
	for (int y = 0; y < chunks_h * 16; y++) {
		uint8_t *base_p = base->pixels + y * stride;
		uint8_t *diff_p = diff->pixels + y * stride;
		int chunk_y = y / 16;
		for (int chunk_x = 0; chunk_x < chunks_w; chunk_x++, base_p += 16 * 4, diff_p += 16 * 4) {
			// XXX: skip memcmp if we already determined chunks differ
			// TODO: benchmark this--is it actually faster?
			int i = chunk_y * chunks_w + chunk_x;
			if (chunk_map[i]) {
				if (!memcmp(base_p, diff_p, 16 * 4))
					continue;
				// XXX: check for case where diff is invisible
				bool diff = false;
				for (int x = 0; x < 16; x++) {
					if (base_p[x*4+3] == 0 && diff_p[x*4+3] == 0)
						continue;
					if (memcmp(base_p+x*4, diff_p+x*4, 4)) {
						diff = true;
						break;
					}
				}
				if (diff)
					chunk_map[i] = 0;
			}
		}
	}

	// zero chunks in `diff` that are identical
	for (int y = 0; y < chunks_h * 16; y++) {
		uint8_t *diff_p = diff->pixels + y * stride;
		int chunk_y = y / 16;
		for (int chunk_x = 0; chunk_x < chunks_w; chunk_x++) {
			if (chunk_map[chunk_y * chunks_w + chunk_x]) {
				memset(diff_p, 0, 16 * 4);
			}
			diff_p += 16 * 4;
		}
	}

	// compress _chunk map
	int cm_fullsize = 4 + chunks_w * chunks_h;
	unsigned long cm_compsize = compressBound(cm_fullsize);
	uint8_t *chunkmap_compressed = xmalloc(cm_compsize);
	int r = compress2(chunkmap_compressed, &cm_compsize, _chunk_map, cm_fullsize,
			Z_BEST_COMPRESSION);
	if (r != Z_OK) {
		WARNING("compress() failed with error code %d", r);
		free(_chunk_map);
		free(chunkmap_compressed);
		return NULL;
	}

	// encode `diff` to QNT
	size_t cg_data_size;
	uint8_t *cg_data = cg_write_mem(diff, ALCG_QNT, &cg_data_size);

	// char[4] magic; // 'dcf '
	// u32 header_size; // not including magic/size
	// u32 version; // = 1
	// u32 width;
	// u32 height;
	// u32 bpp;
	// u32 name_len;
	// u8[name_len] name; // encrypted
	size_t name_len = strlen(base_cg_name);
	size_t dcf_size = 7 * 4 + name_len;

	// char[4] magic; // 'dfdl'
	// u32 section_size; // not including magic/size
	// u32 uncompressed_size;
	// u8[?] compressed_chunkmap;
	size_t dfdl_size = 4 * 3 + cm_compsize;

	// char[4] magic; // 'dcgd'
	// u32 section_size; // not including magic/size
	// u8[?] qnt_data;
	size_t dcgd_size = 4 * 2 + cg_data_size;

	uint8_t *out = xmalloc(dcf_size + dfdl_size + dcgd_size);
	uint8_t *dcf = out;
	memcpy(dcf, "dcf ", 4);
	LittleEndian_putDW(dcf, 4, dcf_size - 8);
	LittleEndian_putDW(dcf, 8, 1);
	LittleEndian_putDW(dcf, 12, base->metrics.w);
	LittleEndian_putDW(dcf, 16, base->metrics.h);
	LittleEndian_putDW(dcf, 20, 32);
	LittleEndian_putDW(dcf, 24, name_len);
	// encode base CG name
	const uint8_t *base_name = (const uint8_t*)base_cg_name;
	uint8_t rot = (name_len % 7) + 1;
	for (int i = 0; i < name_len; i++) {
		dcf[28 + i] = (base_name[i] >> rot) | (base_name[i] << (8-rot));
	}

	uint8_t *dfdl = out + dcf_size;
	memcpy(dfdl, "dfdl", 4);
	LittleEndian_putDW(dfdl, 4, dfdl_size - 8);
	LittleEndian_putDW(dfdl, 8, cm_fullsize);
	memcpy(dfdl + 12, chunkmap_compressed, cm_compsize);

	uint8_t *dcgd = out + dcf_size + dfdl_size;
	memcpy(dcgd, "dcgd", 4);
	LittleEndian_putDW(dcgd, 4, dcgd_size - 8);
	memcpy(dcgd + 8, cg_data, cg_data_size);

	*size_out = dcf_size + dfdl_size + dcgd_size;

	free(cg_data);
	free(chunkmap_compressed);
	free(_chunk_map);
	return out;
}
