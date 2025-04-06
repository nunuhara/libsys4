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

#ifndef SYSTEM4_FNL_H
#define SYSTEM4_FNL_H

#include <stdint.h>
#include <stdio.h>

struct fnl {
	FILE *file;
	uint32_t filesize;
	uint32_t uk;
	uint32_t index_size;
	uint32_t nr_fonts;
	struct fnl_font *fonts;

};

struct fnl_font {
	struct fnl *fnl;
	uint32_t nr_faces;
	struct fnl_font_face *faces;
};

struct fnl_font_face {
	struct fnl_font *font;
	uint32_t height;
	uint32_t uk;
	uint32_t nr_glyphs;
	struct fnl_glyph *glyphs;
};

struct fnl_glyph {
	uint32_t height;
	uint16_t real_width;
	uint32_t data_pos;
	uint32_t data_compsize;
};

struct fnl *fnl_open(const char *path);
void fnl_free(struct fnl *fnl);

unsigned fnl_char_to_index(uint16_t code);
uint16_t fnl_index_to_char(unsigned index);

struct fnl_glyph *fnl_get_glyph(struct fnl_font_face *font, uint16_t code);
uint8_t *fnl_glyph_data(struct fnl *fnl, struct fnl_glyph *g, unsigned long *size);

#endif /* SYSTEM4_FNL_H */
