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

#ifndef SYSTEM4_BUFFER_H
#define SYSTEM4_BUFFER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

struct buffer {
	uint8_t *buf;
	size_t size;
	size_t index;
};

void buffer_init(struct buffer *r, uint8_t *buf, size_t size);
int32_t buffer_read_int32(struct buffer *r);
uint8_t buffer_read_u8(struct buffer *r);
uint16_t buffer_read_u16(struct buffer *r);
float buffer_read_float(struct buffer *r);
/* Read a null-terminated string. */
struct string *buffer_read_string(struct buffer *r);
/* Skip a null-terminated string and returns a pointer to the string. */
char *buffer_skip_string(struct buffer *r);
struct string *buffer_read_pascal_string(struct buffer *r);
struct string *buffer_conv_pascal_string(struct buffer *buf, struct string *(*conv)(const char*,size_t));
void buffer_read_bytes(struct buffer *r, uint8_t *dst, size_t n);
void buffer_skip(struct buffer *r, size_t off);
size_t buffer_remaining(struct buffer *r);
bool buffer_check_bytes(struct buffer *r, const char *data, size_t n);

void buffer_write_int32(struct buffer *b, uint32_t v);
void buffer_write_int32_at(struct buffer *buf, size_t index, uint32_t v);
void buffer_write_int16(struct buffer *b, uint16_t v);
void buffer_write_int8(struct buffer *b, uint8_t v);
void buffer_write_float(struct buffer *b, float f);
void buffer_write_bytes(struct buffer *b, const uint8_t *bytes, size_t len);

/* Write a null-terminated string. */
void buffer_write_string(struct buffer *b, struct string *s);
/* Write a string without a null terminator. */
void buffer_write_cstring(struct buffer *b, const char *s);
/* Write a null-terminated string. */
void buffer_write_cstringz(struct buffer *b, const char *s);
/* Write a length-prefixed string. */
void buffer_write_pascal_string(struct buffer *b, struct string *s);
void buffer_write_pascal_cstring(struct buffer *b, const char *s);

static inline char *buffer_strdata(struct buffer *r)
{
	return (char*)r->buf + r->index;
}

static inline void buffer_seek(struct buffer *r, size_t off)
{
	r->index = off;
}

static inline void buffer_align(struct buffer *r, int p)
{
	r->index = (r->index + (p-1)) & ~(p-1);
}

#endif /* SYSTEM4_BUFFER_H */
