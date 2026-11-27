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
#include <stdio.h>
#include <string.h>
#include "system4.h"
#include "system4/string.h"
#include "system4/utfsjis.h"

struct string EMPTY_STRING = {
	.cow = true,
	.ref = 1,
	.size = 0,
	.text = ""
};

static struct string *alloc_string(int size)
{
	return xmalloc(sizeof(struct string) + size + 1);
}

void free_string(struct string *str)
{
	if (!str->ref)
		ERROR("Double free of string object");
	if (!--str->ref) {
		free(str);
	}
}

static struct string *cow_check(struct string *s)
{
	if (s->cow && s->ref > 1) {
		struct string *out = string_dup(s);
		free_string(s);
		return out;
	}
	if (s->cow)
		s->cow = 0;
	return s;
}

struct string *string_alloc(unsigned int len)
{
	struct string *s = alloc_string(len);
	s->size = len;
	s->ref = 1;
	s->cow = 0;
	s->text[len] = '\0';
	return s;
}

struct string *string_realloc(struct string *s, unsigned int size)
{
	s = xrealloc(cow_check(s), sizeof(struct string) + size + 1);
	s->size = size;
	s->text[size] = '\0';
	return s;
}

struct string *make_string(const char *str, size_t len)
{
	struct string *s = alloc_string(len);
	s->size = len;
	s->ref = 1;
	s->cow = 0;
	memcpy(s->text, str, len);
	s->text[len] = '\0';
	return s;
}

struct string *cstr_to_string(const char *str)
{
	return make_string(str, strlen(str));
}

struct string *string_ref(struct string *s)
{
	s->cow = 1;
	s->ref++;
	return s;
}

struct string *string_dup(const struct string *in)
{
	struct string *out = alloc_string(in->size);
	out->size = in->size;
	out->ref = 1;
	out->cow = 0;
	memcpy(out->text, in->text, in->size + 1);
	return out;
}

struct string *integer_to_string(int n)
{
	char buf[512];
	int len = snprintf(buf, sizeof(buf), "%d", n);
	if (len < 0)
		len = 0;
	else if ((size_t)len >= sizeof(buf))
		len = sizeof(buf) - 1;
	return make_string(buf, len);
}

static void number_zen2han(char *buf)
{
	int dst = 0;
	for (int src = 0; buf[src];) {
		uint8_t b1 = buf[src];
		if (SJIS_2BYTE(b1) && buf[src+1]) {
			uint8_t b2 = buf[src+1];
			int digit_base = sys4_text_is_gbk ? 0xb0 : 0x4f;
			if (b1 == (sys4_text_is_gbk ? 0xa3 : 0x82) && b2 >= digit_base && b2 < digit_base + 10) {
				buf[dst++] = '0' + (b2 - digit_base);
				src += 2;
			} else if (b1 == (sys4_text_is_gbk ? 0xa3 : 0x81) && b2 == (sys4_text_is_gbk ? 0xad : 0x7c)) {
				buf[dst++] = '-';
				src += 2;
			} else if (b1 == (sys4_text_is_gbk ? 0xa3 : 0x81) && b2 == (sys4_text_is_gbk ? 0xae : 0x44)) {
				buf[dst++] = '.';
				src += 2;
			} else if (b1 == (sys4_text_is_gbk ? 0xa1 : 0x81) && b2 == (sys4_text_is_gbk ? 0xa1 : 0x40)) {
				buf[dst++] = ' ';
				src += 2;
			} else {
				buf[dst++] = buf[src++];
				buf[dst++] = buf[src++];
			}
		} else {
			buf[dst++] = buf[src++];
		}
	}
	buf[dst] = '\0';
}

int string_to_integer(struct string *s)
{
	char *buf = xstrdup(s->text);
	number_zen2han(buf);
	int n = atoi(buf);
	free(buf);
	return n;
}

struct string *float_to_string(float f, int precision)
{
	char buf[512];
	int len;

	// System40.exe pushes -1, defaults to 6
	if (precision < 0) {
		precision = 6;
	}

	len = snprintf(buf, sizeof(buf), "%.*f", precision, f);
	if (len < 0)
		len = 0;
	else if ((size_t)len >= sizeof(buf))
		len = sizeof(buf) - 1;
	return make_string(buf, len);
}

struct string *string_concatenate(const struct string *a, const struct string *b)
{
	struct string *s = alloc_string(a->size + b->size);
	s->size = a->size + b->size;
	s->ref = 1;
	s->cow = 0;
	memcpy(s->text, a->text, a->size);
	memcpy(s->text + a->size, b->text, b->size + 1);
	return s;
}

struct string *string_copy(const struct string *s, int index, int len)
{
	if (index < 0)
		index = 0;
	// System 4.x computes end = index + len and scans forward from char 0,
	// copying through end-of-string when end is never reached (e.g. len < 0
	// with index + len < 0, as in Evenicle's string splitter S_GETPART(s, 0, -1)).
	if (len <= 0 && (int64_t)index + (int64_t)len >= 0)
		return make_string("", 0);
	int start = sjis_index(s->text, index);
	if (start < 0)
		return make_string("", 0);
	if (len < 0)
		return make_string(s->text + start, s->size - start);
	int copy_len = sjis_index(s->text + start, len);
	if (copy_len < 0)
		copy_len = s->size - start;

	return make_string(s->text + start, copy_len);
}

void string_append_cstr(struct string **_a, const char *b, size_t b_size)
{
	if (!b_size)
		return;
	struct string *old_a = *_a;
	size_t a_size = old_a->size;
	uintptr_t old_start = (uintptr_t)old_a->text;
	uintptr_t old_end = (uintptr_t)(old_a->text + a_size + 1);
	bool aliased = (uintptr_t)b >= old_start && (uintptr_t)b < old_end;
	size_t b_off = aliased ? (size_t)((uintptr_t)b - old_start) : 0;

	struct string *a = string_realloc(old_a, a_size + b_size);
	const char *src = aliased ? (a->text + b_off) : b;
	memmove(a->text + a_size, src, b_size);
	a->text[a_size + b_size] = '\0';
	*_a = a;
}

void string_append(struct string **_a, const struct string *b)
{
	size_t b_size = b->size;
	if (!b_size)
		return;
	string_append_cstr(_a, b->text, b_size);
}

void string_push_back(struct string **_s, int c)
{
	int bytes = SJIS_2BYTE(c) ? 2 : 1;

	size_t s_size = (*_s)->size;
	struct string *s = string_realloc(*_s, s_size + bytes);
	s->text[s_size] = c & 0xFF;
	if (bytes == 2) {
		s->text[s_size+1] = c >> 8;
	}
	*_s = s;
}

void string_pop_back(struct string **s)
{
	*s = cow_check(*s);
	// get index of last character
	int c = 0;
	for (int i = 0; i < (*s)->size; i++) {
		c = i;
		if (SJIS_2BYTE((*s)->text[i]) && i + 1 < (*s)->size && (*s)->text[i + 1] != '\0') {
			i++;
		}
	}
	(*s)->text[c] = '\0';
	(*s)->size = c;
}

void string_erase(struct string **s, int index)
{
	int bytes;
	if (index < 0)
		index = 0;
	if (index >= (*s)->size)
		return;
	if ((index = sjis_index((*s)->text, index)) < 0 || index >= (*s)->size)
		return;
	bytes = (SJIS_2BYTE((*s)->text[index]) && index + 1 < (*s)->size && (*s)->text[index + 1] != '\0') ? 2 : 1;

	*s = cow_check(*s);
	size_t size = (*s)->size - index - bytes;
	memmove((*s)->text + index, (*s)->text + index + bytes, size);
	(*s)->size -= bytes;
	(*s)->text[(*s)->size] = '\0';
}

void string_clear(struct string *s)
{
	if (!s || s->cow)
		return;
	s->size = 0;
	s->text[0] = '\0';
}

void string_clear_ref(struct string **s)
{
	if (!s)
		return;
	if (!*s) {
		*s = make_string("", 0);
		return;
	}
	if ((*s)->cow) {
		free_string(*s);
		*s = make_string("", 0);
		return;
	}
	(*s)->size = 0;
	(*s)->text[0] = '\0';
}

int string_find(const struct string *haystack, const struct string *needle)
{
	int c = 0;
	for (int i = 0; i < haystack->size; i++, c++) {
		if (!strncmp(haystack->text+i, needle->text, needle->size))
			return c;
		if (SJIS_2BYTE(haystack->text[i]) && i + 1 < haystack->size && haystack->text[i + 1] != '\0') {
			i++;
		}
	}
	return -1;
}

int string_get_char(const struct string *str, int i)
{
	// Comparing with the byte length is weird but this is how System4.0 works.
	if (i < 0 || i > str->size)
		ERROR("String index out of bounds");
	if ((i = sjis_index(str->text, i)) < 0 || i >= str->size)
		return 0;

	if (SJIS_2BYTE(str->text[i]) && i + 1 < str->size && str->text[i + 1] != '\0')
		return (uint8_t)str->text[i] | ((uint8_t)str->text[i+1] << 8);
	return (uint8_t)str->text[i];
}

void string_set_char(struct string **_str, int i, unsigned int c)
{
	struct string *str = *_str = cow_check(*_str);
	int bytes_src, bytes_dst;

	// Comparing with the byte length is weird but this is how System4.0 works.
	if (i < 0 || i >= str->size)
		ERROR("String index out of bounds");
	if ((i = sjis_index(str->text, i)) < 0 || i >= str->size)
		return;

	if (c == 0) {
		// truncate
		str->text[i] = '\0';
		str->size = i;
		return;
	}

	bytes_src = SJIS_2BYTE(c) ? 2 : 1;
	bytes_dst = (SJIS_2BYTE(str->text[i]) && i + 1 < str->size && str->text[i + 1] != '\0') ? 2 : 1;

	if (bytes_src == 1 && bytes_dst == 1) {
		str->text[i] = c;
	} else if (bytes_src == 2 && bytes_dst == 2) {
		str->text[i]   = c & 0xFF;
		str->text[i+1] = (c >> 8) & 0xFF;
	} else if (bytes_src == 1 && bytes_dst == 2) {
		// shrink 1 byte
		str->text[i] = c;
		for (int j = i+1; j < str->size; j++) {
			str->text[j] = str->text[j+1];
		}
		str->size--;
	} else if (bytes_src == 2 && bytes_dst == 1) {
		// grow 1 byte
		str = xrealloc(str, sizeof(struct string) + str->size + 2);
		str->size++;
		*_str = str;
		for (int j = str->size; j > i; j--) {
			str->text[j] = str->text[j-1];
		}
		str->text[i]   = c & 0xFF;
		str->text[i+1] = (c >> 8) & 0xFF;
	}
}

#define DIGIT_MAX 512

static int number_han2zen(char *buf, size_t size)
{
	if (!size) return 0;
	char *tmp = xmalloc(size);
	int i = 0;
	for (char *p = buf; *p && (size_t)i + 2 < size; p++) {
		switch (*p) {
		case '0': case '1': case '2': case '3': case '4':
		case '5': case '6': case '7': case '8': case '9':
			tmp[i++] = sys4_text_is_gbk ? 0xa3 : 0x82;
			tmp[i++] = (sys4_text_is_gbk ? 0xb0 : 0x4f) + (*p - '0');
			break;
		case '-':
			tmp[i++] = sys4_text_is_gbk ? 0xa3 : 0x81;
			tmp[i++] = sys4_text_is_gbk ? 0xad : 0x7c;
			break;
		case '.':
			tmp[i++] = sys4_text_is_gbk ? 0xa3 : 0x81;
			tmp[i++] = sys4_text_is_gbk ? 0xae : 0x44;
			break;
		case ' ':
			tmp[i++] = sys4_text_is_gbk ? 0xa1 : 0x81;
			tmp[i++] = sys4_text_is_gbk ? 0xa1 : 0x40;
			break;
		default:
			tmp[i++] = *p;
		}
	}
	tmp[i] = '\0';

	memcpy(buf, tmp, i+1);
	free(tmp);
	return i;
}

int int_to_cstr(char *buf, size_t size, int v, int figures, bool zero_pad, bool zenkaku)
{
	if (!size) return 0;
	char fmt[64];
	int i = 0;

	// prepare format string for snprintf
	fmt[i++] = '%';
	if (figures > 0) {
		if (zero_pad)
			fmt[i++] = '0';
		i += snprintf(fmt+i, 64-i, "%d", figures);
	}
	fmt[i++] = 'd';
	fmt[i] = '\0';

	i = snprintf(buf, size, fmt, v);
	i = i < 0 ? 0 : min((size_t)i, size - 1);
	buf[i] = '\0';

	if (zenkaku)
		i = number_han2zen(buf, size);

	return i;
}

int float_to_cstr(char *buf, size_t size, float v, int figures, bool zero_pad, int precision, bool zenkaku)
{
	if (!size) return 0;
	char fmt[64];
	int i = 0;

	fmt[i++] = '%';
	if (figures > 0) {
		if (zero_pad)
			fmt[i++] = '0';
		i += snprintf(fmt+i, 64-i, "%d", figures);
	}
	fmt[i++] = '.';
	i += snprintf(fmt+i, 64-i, "%d", precision);
	fmt[i++] = 'f';
	fmt[i] = '\0';

	i = snprintf(buf, size, fmt, v);
	i = i < 0 ? 0 : min((size_t)i, size - 1);
	buf[i] = '\0';

	if (zenkaku) {
		i = number_han2zen(buf, size);
		// XXX: bug in System40.exe
		if ((size_t)i + 1 < size) buf[i++] = 'F';
		buf[i] = '\0';
	}
	return i;
}
