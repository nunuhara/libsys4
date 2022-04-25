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

#include <stdint.h>
#include <stdlib.h>
#include <math.h>
#include <zlib.h>
#include "system4.h"
#include "system4/buffer.h"
#include "system4/file.h"
#include "system4/fnl.h"
#include "system4/utfsjis.h"
#include "little_endian.h"

/*
 * NOTE: FNL glyphs are indexed according to the sequential order of code points
 *       encoded by Shift-JIS, beginning with the ASCII space character (0x20).
 */

static unsigned int sjis_code_to_index(uint16_t code)
{
	// one byte
	if (code < 0x20)
		return 0;
	if (code < 0x7f)
		return code - 0x20;
	if (code < 0xa1)
		return 0;
	if (code < 0xe0)
		return code - 0x42;

	// two byte
	uint8_t fst = code >> 8;
	uint8_t snd = code & 0xFF;

	if (snd < 0x40 || snd == 0x7f || snd > 0xfc)
		return 0;

	// index of first byte (within allowable range)
	unsigned fst_index = 0;
	// index of second byte (within allowable range)
	unsigned snd_index = snd - (0x40 + (snd > 0x7f ? 1 : 0));

	if (fst < 0x81)
		return 0;
	else if (fst < 0xa0)
		fst_index = fst - 0x81;
	else if (fst < 0xe0)
		return 0;
	else if (fst < 0xfd)
		fst_index = (fst - 0xe0) + 31;
	else
		return 0;

	return 158 + fst_index*188 + snd_index;
}

static possibly_unused uint16_t index_to_sjis_code(unsigned index)
{
	if (index < 95)
		return index + 0x20;
	if (index < 158)
		return (index - 95) + 0xA1;

	index -= 158;

	// NOTE: 188  = number of code points encoded per first-byte in SJIS
	//       0x81 = first valid SJIS first-byte
	//       0xa0 = beginning of invalid first-bytes
	//       31   = number of invalid first-bytes beginning at 0xa0
	uint16_t fst = 0x81 + index / 188;
	if (fst >= 0xa0)
		fst += 31;

	// NOTE: 0x40 = first valid SJIS second-byte
	//       0x7f = invalid as a second-byte
	uint16_t snd = 0x40 + index % 188;
	if (snd >= 0x7f)
		snd += 1;

	return (fst << 8) | snd;
}

struct fnl_glyph *fnl_get_glyph(struct fnl_font_face *font, uint16_t code)
{
	unsigned int index = sjis_code_to_index(code);

	if (index >= font->nr_glyphs)
		index = 0;
	if (!font->glyphs[index].data_pos)
		index = 0;

	return &font->glyphs[index];
}

static struct fnl_rendered_glyph *render_glyph_fullsize(struct fnl_font_face *face,
		struct fnl_glyph *glyph)
{
	// decompress glyph bitmap
	unsigned long data_size;
	uint8_t *data = fnl_glyph_data(face->font->fnl, glyph, &data_size);

	const int glyph_w = (data_size*8) / face->height;
	const int glyph_h = face->height;

	struct fnl_rendered_glyph *out = xmalloc(sizeof(struct fnl_rendered_glyph));
	out->width = glyph_w;
	out->height = glyph_h;
	out->advance = glyph->real_width;
	out->pixels = xmalloc(glyph_w * glyph_h);
	out->data = NULL;

	// expand 1-bit bitmap to 8-bit
	for (unsigned i = 0; i < glyph_w * glyph_h; i++) {
		unsigned row = (glyph_h - 1) - i / glyph_w;
		unsigned col = i % glyph_w;
		bool on = data[i/8] & (1 << (7 - i % 8));
		out->pixels[row*glyph_w + col] = on ? 255 : 0;
	}

	free(data);
	return out;
}

static struct fnl_rendered_glyph *render_glyph_downscaled(struct fnl_rendered_glyph *fullsize,
		unsigned denominator)
{
	struct fnl_rendered_glyph *out = xmalloc(sizeof(struct fnl_rendered_glyph));
	out->width = fullsize->width / denominator;
	out->height = fullsize->height / denominator;
	out->advance = fullsize->advance / denominator;
	out->pixels = xmalloc(out->width * out->height);
	out->data = NULL;

	unsigned *acc = xcalloc(out->width * out->height, sizeof(unsigned));

	// sample each pixel in `denominator`-sized blocks and compute the average
	// TODO: no need to sample every single pixel; 4 should be fine?
	const unsigned span = out->width;
	const unsigned size = span * out->height;
	for (unsigned i = 0; i < size; i++) {
		unsigned dst_row = i / span;
		unsigned dst_col = i % span;
		for (unsigned r = 0; r < denominator; r++) {
			unsigned src_row = dst_row * denominator + r;
			for (unsigned c = 0; c < denominator; c++) {
				unsigned src_col = dst_col * denominator + c;
				acc[i] += fullsize->pixels[src_row*fullsize->width + src_col];
			}
		}
		out->pixels[i] = acc[i] / (denominator * denominator);
	}

	free(acc);
	return out;
}

struct fnl_rendered_glyph *fnl_render_glyph(struct fnl_font_size *size, uint16_t code)
{
	unsigned int index = sjis_code_to_index(code);

	if (index >= size->face->nr_glyphs)
		return NULL;
	if (size->cache && size->cache[index])
		return size->cache[index];
	if (!size->face->glyphs[index].data_pos)
		return NULL;

	if (!size->cache) {
		size->cache = xcalloc(size->face->nr_glyphs, sizeof(struct fnl_rendered_glyph*));
	}

	// render glyph at full size
	if (size->denominator == 1) {
		size->cache[index] = render_glyph_fullsize(size->face, &size->face->glyphs[index]);
	}
	// render downscaled glyph
	else {
		struct fnl_rendered_glyph *full = fnl_render_glyph(size->fullsize, code);
		size->cache[index] = render_glyph_downscaled(full, size->denominator);
	}

	return size->cache[index];
}

/*
 * Get the closest font size.
 */
struct fnl_font_size *fnl_get_font_size(struct fnl_font *font, float size)
{
	float min_diff = 9999;
	struct fnl_font_size *closest = &font->sizes[0];
	for (unsigned i = 0; i < font->nr_sizes; i++) {
		float diff = fabsf(font->sizes[i].size - size);
		if (diff < min_diff) {
			min_diff = diff;
			closest = &font->sizes[i];
		}
	}
	return closest;
}

struct fnl_font_size *fnl_get_font_size_round_down(struct fnl_font *font, float size)
{
	float min_diff = 9999;
	struct fnl_font_size *closest = &font->sizes[0];
	for (unsigned i = 0; i < font->nr_sizes; i++) {
		if (font->sizes[i].size > size)
			continue;
		float diff = fabsf(font->sizes[i].size - size);
		if (diff < min_diff) {
			min_diff = diff;
			closest = &font->sizes[i];
		}
	}
	return closest;
}

static void fnl_read_glyph(struct buffer *r, uint32_t height, struct fnl_glyph *dst)
{
	dst->height = height;
	dst->real_width = buffer_read_u16(r);
	dst->data_pos = buffer_read_int32(r);
	dst->data_compsize = buffer_read_int32(r);
}

uint8_t *fnl_glyph_data(struct fnl *fnl, struct fnl_glyph *g, unsigned long *size)
{
	if (!g->data_pos) {
		*size = 0;
		return NULL;
	}

	*size = g->height * g->height * 4; // FIXME: determine real bound
	uint8_t *data = xmalloc(*size);
	int rv = uncompress(data, size, fnl->data + g->data_pos, g->data_compsize);
	if (rv != Z_OK) {
		if (rv == Z_BUF_ERROR)
			ERROR("uncompress failed: Z_BUF_ERROR");
		else if (rv == Z_MEM_ERROR)
			ERROR("uncompress failed: Z_MEM_ERROR");
		else if (rv == Z_DATA_ERROR)
			ERROR("uncompress failed: Z_DATA_ERROR");
	}
	return data;
}

static void fnl_read_font_face(struct buffer *r, struct fnl_font_face *dst)
{
	dst->height    = buffer_read_int32(r);
	dst->uk        = buffer_read_int32(r);
	dst->nr_glyphs = buffer_read_int32(r);

	dst->glyphs = xcalloc(dst->nr_glyphs, sizeof(struct fnl_glyph));
	for (size_t i = 0; i < dst->nr_glyphs; i++) {
		fnl_read_glyph(r, dst->height, &dst->glyphs[i]);
	}

	dst->_sizes[0] = dst->height;
	for (unsigned i = 1; i < 12; i++) {
		dst->_sizes[i] = dst->height / (float)(i+1);
	}
}

static void fnl_read_font(struct buffer *r, struct fnl_font *dst)
{
	dst->nr_faces = buffer_read_int32(r);

	dst->faces = xcalloc(dst->nr_faces, sizeof(struct fnl_font_face));
	for (size_t i = 0; i < dst->nr_faces; i++) {
		dst->faces[i].font = dst;
		fnl_read_font_face(r, &dst->faces[i]);
	}

	dst->nr_sizes = dst->nr_faces * 12;
	dst->sizes = xcalloc(dst->nr_sizes, sizeof(struct fnl_font_size));
	for (unsigned face = 0, s = 0; face < dst->nr_faces; face++) {
		unsigned fullsize = s;
		for (unsigned i = 0; i < 12; i++) {
			dst->sizes[s++] = (struct fnl_font_size) {
				.face = &dst->faces[face],
				.fullsize = &dst->sizes[fullsize],
				.size = dst->faces[face]._sizes[i],
				.denominator = i+1
			};
		}
	}

}

struct fnl *fnl_open(const char *path)
{
	size_t filesize;
	struct fnl *fnl = xcalloc(1, sizeof(struct fnl));
	fnl->data = file_read(path, &filesize);

	if (fnl->data[0] != 'F' || fnl->data[1] != 'N' || fnl->data[2] != 'A' || fnl->data[3] != '\0')
		goto err;

	struct buffer r;
	buffer_init(&r, fnl->data, filesize);
	buffer_skip(&r, 4);
	fnl->uk = buffer_read_int32(&r);
	if (fnl->uk != 0)
		WARNING("Unexpected value for fnl->uk: %d", fnl->uk);

	fnl->filesize = buffer_read_int32(&r);
	fnl->data_offset = buffer_read_int32(&r);
	fnl->nr_fonts = buffer_read_int32(&r);

	fnl->fonts = xcalloc(fnl->nr_fonts, sizeof(struct fnl_font));
	for (size_t i = 0; i < fnl->nr_fonts; i++) {
		fnl->fonts[i].fnl = fnl;
		fnl_read_font(&r, &fnl->fonts[i]);
	}
	return fnl;
err:
	free(fnl->data);
	free(fnl);
	return NULL;
}

static void free_cache(struct fnl_rendered_glyph **cache, unsigned cache_size, void(*free_data)(void*))
{
	if (!cache)
		return;
	for (unsigned i = 0; i < cache_size; i++) {
		if (!cache[i])
			continue;
		if (cache[i]->data && free_data)
			free_data(cache[i]->data);
		free(cache[i]->pixels);
		free(cache[i]);
	}
	free(cache);
}

void fnl_free(struct fnl *fnl, void(*free_data)(void*))
{
	for (size_t font_i = 0; font_i < fnl->nr_fonts; font_i++) {
		struct fnl_font *font = &fnl->fonts[font_i];
		for (unsigned size_i = 0; size_i < font->nr_sizes; size_i++) {
			struct fnl_font_size *size = &font->sizes[size_i];
			free_cache(size->cache, size->face->nr_glyphs, free_data);
		}
		free(font->sizes);

		for (size_t face = 0; face < font->nr_faces; face++) {
			free(font->faces[face].glyphs);
		}
		free(font->faces);

	}
	free(fnl->fonts);
	free(fnl->data);
	free(fnl);
}
