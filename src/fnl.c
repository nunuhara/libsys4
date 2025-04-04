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

unsigned fnl_char_to_index(uint16_t code)
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

uint16_t fnl_index_to_char(unsigned index)
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
	unsigned int index = fnl_char_to_index(code);

	if (index >= font->nr_glyphs)
		index = 0;
	if (!font->glyphs[index].data_pos)
		index = 0;

	return &font->glyphs[index];
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

	fseek(fnl->file, g->data_pos, SEEK_SET);
	uint8_t *compressed_data = xmalloc(g->data_compsize);
	if (fread(compressed_data, g->data_compsize, 1, fnl->file) != 1)
		ERROR("Failed to read compressed data");

	int rv = uncompress(data, size, compressed_data, g->data_compsize);
	free(compressed_data);

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
}

static void fnl_read_font(struct buffer *r, struct fnl_font *dst)
{
	dst->nr_faces = buffer_read_int32(r);

	dst->faces = xcalloc(dst->nr_faces, sizeof(struct fnl_font_face));
	for (size_t i = 0; i < dst->nr_faces; i++) {
		dst->faces[i].font = dst;
		fnl_read_font_face(r, &dst->faces[i]);
	}
}

struct fnl *fnl_open(const char *path)
{
	uint8_t *index_buf = NULL;
	struct fnl *fnl = xcalloc(1, sizeof(struct fnl));
	fnl->file = file_open_utf8(path, "rb");
	if (!fnl->file)
		goto err;

	uint8_t header[16];
	if (fread(header, sizeof(header), 1, fnl->file) != 1)
		goto err;
	struct buffer r;
	buffer_init(&r, header, sizeof(header));
	if (!buffer_check_bytes(&r, "FNA\0", 4))
		goto err;

	fnl->uk = buffer_read_int32(&r);
	if (fnl->uk != 0)
		WARNING("Unexpected value for fnl->uk: %d", fnl->uk);

	fnl->filesize = buffer_read_int32(&r);
	fnl->index_size = buffer_read_int32(&r);

	index_buf = xmalloc(fnl->index_size);
	if (fread(index_buf, fnl->index_size, 1, fnl->file) != 1)
		goto err;
	buffer_init(&r, index_buf, fnl->index_size);

	fnl->nr_fonts = buffer_read_int32(&r);
	fnl->fonts = xcalloc(fnl->nr_fonts, sizeof(struct fnl_font));
	for (size_t i = 0; i < fnl->nr_fonts; i++) {
		fnl->fonts[i].fnl = fnl;
		fnl_read_font(&r, &fnl->fonts[i]);
	}
	if (buffer_remaining(&r) != 0)
		WARNING("Buffer not empty after reading fonts: %zu bytes left", buffer_remaining(&r));
	free(index_buf);
	return fnl;
err:
	if (index_buf)
		free(index_buf);
	if (fnl->file)
		fclose(fnl->file);
	free(fnl);
	return NULL;
}

void fnl_free(struct fnl *fnl)
{
	for (size_t font_i = 0; font_i < fnl->nr_fonts; font_i++) {
		struct fnl_font *font = &fnl->fonts[font_i];
		for (size_t face = 0; face < font->nr_faces; face++) {
			free(font->faces[face].glyphs);
		}
		free(font->faces);
	}
	free(fnl->fonts);
	fclose(fnl->file);
	free(fnl);
}
