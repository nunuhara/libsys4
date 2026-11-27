/*
 * utfsjis.c -- utf-8/sjis related function
 *
 * Copyright (C) 1997 Yutaka OIWA <oiwa@is.s.u-tokyo.ac.jp>
 *
 * written for Satoshi KURAMOCHI's "eplaymidi"
 *                                   <satoshi@ueda.info.waseda.ac.jp>
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
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

#include "system4.h"
#include "system4/file.h"
#include "system4/utfsjis.h"
#include "system4/s2utbl.h"
#include "system4/string.h"
#include "gbk_table.h"

bool sys4_text_is_gbk = false;
bool sys4_source_is_utf8 = false;

/* Symbols used by UTF-8 releases that GBK lacks, parked in the GBK user-defined area. */
static const struct { uint16_t gbk; uint16_t u; } gbk_extras[] = {
	{ 0xaaa1, 0x266a }, // ♪
	{ 0xaaa2, 0x30fb }, // ・
	{ 0xaaa3, 0x2764 }, // ❤
	{ 0xaaa4, 0x2665 }, // ♥
};

static int gbk_lookup(uint8_t lead, uint8_t trail)
{
	int u = gbk_to_unicode[lead - 0x81][trail - 0x40];
	if (u)
		return u;
	uint16_t code = (uint16_t)((lead << 8) | trail);
	for (size_t i = 0; i < sizeof(gbk_extras) / sizeof(gbk_extras[0]); i++) {
		if (gbk_extras[i].gbk == code)
			return gbk_extras[i].u;
	}
	return 0;
}
static uint16_t unicode_to_gbk[65536];
static uint16_t unicode_to_sjis_tbl[65536];

static void init_unicode_to_gbk(void)
{
	for (int lead = 0x81; lead <= 0xfe; lead++) {
		for (int trail = 0x40; trail <= 0xfe; trail++) {
			int code = gbk_to_unicode[lead - 0x81][trail - 0x40];
			if (code && code < 65536)
				unicode_to_gbk[code] = (uint16_t)((lead << 8) | trail);
		}
	}
	for (size_t i = 0; i < sizeof(gbk_extras) / sizeof(gbk_extras[0]); i++) {
		if (!unicode_to_gbk[gbk_extras[i].u])
			unicode_to_gbk[gbk_extras[i].u] = gbk_extras[i].gbk;
	}
}

static void init_unicode_to_sjis(void)
{
	for (int b1 = 0x81; b1 <= 0xfc; b1++) {
		if (b1 >= 0xa0 && b1 <= 0xdf)
			continue;
		for (int b2 = 0x40; b2 <= 0xfc; b2++) {
			int u = s2u[b1 - 0x80][b2 - 0x40];
			if (u > 0 && u < 65536 && !unicode_to_sjis_tbl[u])
				unicode_to_sjis_tbl[u] = (uint16_t)((b1 << 8) | b2);
		}
	}
}

#ifdef _WIN32
static INIT_ONCE unicode_to_gbk_once = INIT_ONCE_STATIC_INIT;
static INIT_ONCE unicode_to_sjis_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK init_unicode_to_gbk_win32(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
	(void)once; (void)param; (void)ctx;
	init_unicode_to_gbk();
	return TRUE;
}

static BOOL CALLBACK init_unicode_to_sjis_win32(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
	(void)once; (void)param; (void)ctx;
	init_unicode_to_sjis();
	return TRUE;
}

static void ensure_unicode_to_gbk(void)
{
	InitOnceExecuteOnce(&unicode_to_gbk_once, init_unicode_to_gbk_win32, NULL, NULL);
}

static void ensure_unicode_to_sjis(void)
{
	InitOnceExecuteOnce(&unicode_to_sjis_once, init_unicode_to_sjis_win32, NULL, NULL);
}
#else
static pthread_once_t unicode_to_gbk_once = PTHREAD_ONCE_INIT;
static pthread_once_t unicode_to_sjis_once = PTHREAD_ONCE_INIT;

static void ensure_unicode_to_gbk(void)
{
	pthread_once(&unicode_to_gbk_once, init_unicode_to_gbk);
}

static void ensure_unicode_to_sjis(void)
{
	pthread_once(&unicode_to_sjis_once, init_unicode_to_sjis);
}
#endif

#define MAI_WORD_TABLE_SIZE 65536
#define MAI_NR_ROWS 128
#define MAI_NR_COLS 192

static bool sys4_mai_active = false;
static uint16_t mai_s2u[MAI_NR_ROWS][MAI_NR_COLS];
static uint16_t mai_u2s[MAI_WORD_TABLE_SIZE];

void sys4_reset_mai_word_table(void)
{
	sys4_mai_active = false;
	memset(mai_s2u, 0, sizeof(mai_s2u));
	memset(mai_u2s, 0, sizeof(mai_u2s));
}

bool sys4_has_mai_word_table(void)
{
	return sys4_mai_active;
}

static bool mai_valid_slot_lead(int hi)
{
	return (hi >= 0x81 && hi <= 0x9f) || (hi >= 0xe0 && hi <= 0xef);
}

static bool mai_valid_slot_trail(int lo)
{
	return (lo >= 0x40 && lo <= 0x7e) || (lo >= 0x80 && lo <= 0x9e) || (lo >= 0xa0 && lo <= 0xfc);
}

/* Build mai.dll-compatible custom Shift-JIS <-> UCS-2 mapping tables from
 * a 65536-byte word.mai BMP bitmap without mutating the base CP932 tables. */
bool sys4_load_mai_word_table_mem(const void *data, size_t size)
{
	if (!data || size != MAI_WORD_TABLE_SIZE)
		return false;

	const uint8_t *raw = (const uint8_t *)data;
	bool has_entry = false;
	for (int u = 0x80; u < MAI_WORD_TABLE_SIZE; u++) {
		if (raw[u]) {
			has_entry = true;
			break;
		}
	}
	if (!has_entry)
		return false;

	uint8_t *needed = xmalloc(MAI_WORD_TABLE_SIZE);
	uint8_t *free_slots = xcalloc(1, MAI_WORD_TABLE_SIZE);
	uint16_t *base_u2s = xcalloc(MAI_WORD_TABLE_SIZE, sizeof(uint16_t));
	for (int u = 0; u < MAI_WORD_TABLE_SIZE; u++)
		needed[u] = raw[u] ? 1 : 0;

	sys4_reset_mai_word_table();

	for (int c = 0xa1; c <= 0xdf; c++)
		base_u2s[0xff60 + (c - 0xa0)] = (uint16_t)c;

	/* Mirror mai.dll: retain CP932 in 0x81..0xEC, clear 0xED00..0xFCFF,
	 * and drop s2utbl.h's 0x30FB placeholder from unmapped cells. */
	for (int b1 = 0x81; b1 <= 0xec; b1++) {
		if (b1 >= 0xa0 && b1 <= 0xdf)
			continue;
		for (int b2 = 0x40; b2 <= 0xfc; b2++) {
			if (b2 == 0x7f)
				continue;
			uint16_t u = (uint16_t)s2u[b1 - 0x80][b2 - 0x40];
			uint16_t sjis = (uint16_t)((b1 << 8) | b2);
			if (u == 0x30fb && sjis != 0x8145)
				continue;
			mai_s2u[b1 - 0x80][b2 - 0x40] = u;
			if (u >= 0x80)
				base_u2s[u] = sjis;
		}
	}

	for (int hi = 0x88; hi <= 0xef; hi++) {
		if (!mai_valid_slot_lead(hi))
			continue;
		for (int lo = 0x40; lo <= 0xfc; lo++) {
			if (!mai_valid_slot_trail(lo))
				continue;
			int slot = (hi << 8) | lo;
			if (slot >= 0x889f)
				free_slots[slot] = 1;
		}
	}

	/* Pass 1: preserve existing CP932 codes required by word.mai. */
	for (int u = 0x80; u < MAI_WORD_TABLE_SIZE; u++) {
		if (needed[u] && base_u2s[u]) {
			uint16_t s = base_u2s[u];
			mai_u2s[u] = s;
			free_slots[s] = 0;
			needed[u] = 0;
		}
	}

	/* Pass 2: assign unmapped word.mai characters into unused 2-byte slots. */
	int slot = 0x889f;
	for (int u = 0x80; u < MAI_WORD_TABLE_SIZE; u++) {
		if (!needed[u])
			continue;
		while (slot < MAI_WORD_TABLE_SIZE && !free_slots[slot])
			slot++;
		if (slot >= MAI_WORD_TABLE_SIZE) {
			free(base_u2s);
			free(free_slots);
			free(needed);
			sys4_reset_mai_word_table();
			return false;
		}
		mai_u2s[u] = (uint16_t)slot;
		mai_s2u[(slot >> 8) - 0x80][(slot & 0xff) - 0x40] = (uint16_t)u;
		free_slots[slot] = 0;
		slot++;
	}

	/* Pass 3: populate reverse lookup for unallocated surviving CP932 chars. */
	for (int c = 0xa1; c <= 0xdf; c++) {
		uint16_t u = (uint16_t)(0xff60 + (c - 0xa0));
		if (!mai_u2s[u])
			mai_u2s[u] = (uint16_t)c;
	}
	for (int b1 = 0x81; b1 <= 0xef; b1++) {
		if (b1 >= 0xa0 && b1 <= 0xdf)
			continue;
		for (int b2 = 0x40; b2 <= 0xfc; b2++) {
			if (b2 == 0x7f)
				continue;
			uint16_t u = mai_s2u[b1 - 0x80][b2 - 0x40];
			if (u >= 0x80 && !mai_u2s[u])
				mai_u2s[u] = (uint16_t)((b1 << 8) | b2);
		}
	}

	free(base_u2s);
	free(free_slots);
	free(needed);
	sys4_mai_active = true;
	return true;
}

bool sys4_load_mai_word_table(const char *path)
{
	if (!path || !*path)
		return false;
	size_t size = 0;
	void *buf = file_read(path, &size);
	if (!buf)
		return false;
	bool ok = sys4_load_mai_word_table_mem(buf, size);
	free(buf);
	return ok;
}

/* Load a Longinus CHS.TBL remap table (up to 65536 little-endian uint16 entries
 * mapping 2-byte Shift-JIS codepoints -> GBK byte pairs) into the custom Shift-JIS
 * render table without mutating base CP932 archive/path conversions. */
bool sys4_load_chs_tbl_mem(const void *data, size_t size)
{
	if (!data || size < 2 || (size % 2) != 0 || size > 131072)
		return false;

	const uint8_t *raw = (const uint8_t *)data;
	size_t count = size / 2;

	sys4_reset_mai_word_table();

	// Seed with base CP932 table so unmapped punctuation/kana remain intact.
	for (int b1 = 0x81; b1 <= 0xfc; b1++) {
		if (b1 >= 0xa0 && b1 <= 0xdf)
			continue;
		for (int b2 = 0x40; b2 <= 0xfc; b2++) {
			if (b2 == 0x7f)
				continue;
			uint16_t u = (uint16_t)s2u[b1 - 0x80][b2 - 0x40];
			uint16_t sjis = (uint16_t)((b1 << 8) | b2);
			if (u == 0x30fb && sjis != 0x8145)
				continue;
			mai_s2u[b1 - 0x80][b2 - 0x40] = u;
		}
	}

	size_t mapped_count = 0;
	for (int b1 = 0x81; b1 <= 0xfc; b1++) {
		if (b1 >= 0xa0 && b1 <= 0xdf)
			continue;
		for (int b2 = 0x40; b2 <= 0xfc; b2++) {
			if (b2 == 0x7f)
				continue;
			size_t code = ((size_t)b1 << 8) | (size_t)b2;
			if (code >= count)
				continue;
			uint16_t mapped = (uint16_t)(raw[code * 2] | ((uint16_t)raw[code * 2 + 1] << 8));
			// Skip empty slots and '?' (0x003f) fallback placeholders so CP932 symbols
			// absent from GBK (e.g. ・ 0x8145, ♪ 0x81f4) remain legible.
			if (!mapped || mapped == 0x003f)
				continue;
			int u = 0;
			if (mapped <= 0x7f) {
				u = mapped;
			} else {
				uint8_t hi = (uint8_t)(mapped >> 8);
				uint8_t lo = (uint8_t)(mapped & 0xff);
				if (hi >= 0x81 && hi <= 0xfe && lo >= 0x40 && lo <= 0xfe)
					u = gbk_lookup(hi, lo);
			}
			if (u > 0 && u < 65536) {
				mai_s2u[b1 - 0x80][b2 - 0x40] = (uint16_t)u;
				if (!mai_u2s[u])
					mai_u2s[u] = (uint16_t)code;
				mapped_count++;
			}
		}
	}

	if (!mapped_count) {
		sys4_reset_mai_word_table();
		return false;
	}

	for (int c = 0xa1; c <= 0xdf; c++) {
		uint16_t u = (uint16_t)(0xff60 + (c - 0xa0));
		if (!mai_u2s[u])
			mai_u2s[u] = (uint16_t)c;
	}
	for (int b1 = 0x81; b1 <= 0xfc; b1++) {
		if (b1 >= 0xa0 && b1 <= 0xdf)
			continue;
		for (int b2 = 0x40; b2 <= 0xfc; b2++) {
			if (b2 == 0x7f)
				continue;
			uint16_t u = mai_s2u[b1 - 0x80][b2 - 0x40];
			if (u >= 0x80 && !mai_u2s[u])
				mai_u2s[u] = (uint16_t)((b1 << 8) | b2);
		}
	}

	sys4_mai_active = true;
	return true;
}

bool sys4_load_chs_tbl(const char *path)
{
	if (!path || !*path)
		return false;
	size_t size = 0;
	void *buf = file_read(path, &size);
	if (!buf)
		return false;
	bool ok = sys4_load_chs_tbl_mem(buf, size);
	free(buf);
	return ok;
}

static void uif_skip_ws(const uint8_t **p, const uint8_t *end)
{
	while (*p < end) {
		uint8_t c = **p;
		if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
			(*p)++;
		else
			break;
	}
}

static int uif_hex_val(uint8_t c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

static bool uif_parse_hex4(const uint8_t **p, const uint8_t *end, uint16_t *out)
{
	if (end - *p < 4)
		return false;
	uint16_t v = 0;
	for (int i = 0; i < 4; i++) {
		int h = uif_hex_val((*p)[i]);
		if (h < 0)
			return false;
		v = (uint16_t)((v << 4) | h);
	}
	*p += 4;
	*out = v;
	return true;
}

static void uif_append_utf8(uint8_t **buf, size_t *len, size_t *cap, uint32_t cp)
{
	if (*len + 5 > *cap) {
		*cap = (*cap < 64) ? 64 : (*cap * 2);
		*buf = xrealloc(*buf, *cap);
	}
	if (cp <= 0x7f) {
		(*buf)[(*len)++] = (uint8_t)cp;
	} else if (cp <= 0x7ff) {
		(*buf)[(*len)++] = (uint8_t)(0xc0 | (cp >> 6));
		(*buf)[(*len)++] = (uint8_t)(0x80 | (cp & 0x3f));
	} else if (cp <= 0xffff) {
		(*buf)[(*len)++] = (uint8_t)(0xe0 | (cp >> 12));
		(*buf)[(*len)++] = (uint8_t)(0x80 | ((cp >> 6) & 0x3f));
		(*buf)[(*len)++] = (uint8_t)(0x80 | (cp & 0x3f));
	} else if (cp <= 0x10ffff) {
		(*buf)[(*len)++] = (uint8_t)(0xf0 | (cp >> 18));
		(*buf)[(*len)++] = (uint8_t)(0x80 | ((cp >> 12) & 0x3f));
		(*buf)[(*len)++] = (uint8_t)(0x80 | ((cp >> 6) & 0x3f));
		(*buf)[(*len)++] = (uint8_t)(0x80 | (cp & 0x3f));
	}
}

static bool uif_parse_json_string(const uint8_t **p, const uint8_t *end, char **out_str, size_t *out_len)
{
	uif_skip_ws(p, end);
	if (*p >= end || **p != '"')
		return false;
	(*p)++;

	size_t cap = 64, len = 0;
	uint8_t *buf = xmalloc(cap);

	while (*p < end) {
		uint8_t c = *(*p)++;
		if (c == '"') {
			buf[len] = '\0';
			if (out_str) {
				*out_str = (char *)buf;
				if (out_len) *out_len = len;
			} else {
				free(buf);
			}
			return true;
		}
		if (c == '\\') {
			if (*p >= end) {
				free(buf);
				return false;
			}
			uint8_t esc = *(*p)++;
			switch (esc) {
			case '"':  case '\\': case '/': uif_append_utf8(&buf, &len, &cap, esc); break;
			case 'b':  uif_append_utf8(&buf, &len, &cap, '\b'); break;
			case 'f':  uif_append_utf8(&buf, &len, &cap, '\f'); break;
			case 'n':  uif_append_utf8(&buf, &len, &cap, '\n'); break;
			case 'r':  uif_append_utf8(&buf, &len, &cap, '\r'); break;
			case 't':  uif_append_utf8(&buf, &len, &cap, '\t'); break;
			case 'u': {
				uint16_t hi = 0;
				if (!uif_parse_hex4(p, end, &hi)) {
					free(buf);
					return false;
				}
				uint32_t cp = hi;
				if (hi >= 0xd800 && hi <= 0xdbff && end - *p >= 6 && (*p)[0] == '\\' && (*p)[1] == 'u') {
					const uint8_t *save = *p;
					*p += 2;
					uint16_t lo = 0;
					if (uif_parse_hex4(p, end, &lo) && lo >= 0xdc00 && lo <= 0xdfff)
						cp = 0x10000 + (((uint32_t)(hi - 0xd800) << 10) | (uint32_t)(lo - 0xdc00));
					else
						*p = save;
				}
				uif_append_utf8(&buf, &len, &cap, cp);
				break;
			}
			default:
				free(buf);
				return false;
			}
		} else {
			if (len + 2 > cap) {
				cap *= 2;
				buf = xrealloc(buf, cap);
			}
			buf[len++] = c;
		}
	}
	free(buf);
	return false;
}

static bool uif_skip_json_value(const uint8_t **p, const uint8_t *end, int depth)
{
	if (depth > 32)
		return false;
	uif_skip_ws(p, end);
	if (*p >= end)
		return false;
	uint8_t c = **p;
	if (c == '"') {
		return uif_parse_json_string(p, end, NULL, NULL);
	} else if (c == '{') {
		(*p)++;
		uif_skip_ws(p, end);
		if (*p < end && **p == '}') {
			(*p)++;
			return true;
		}
		while (*p < end) {
			if (!uif_parse_json_string(p, end, NULL, NULL))
				return false;
			uif_skip_ws(p, end);
			if (*p >= end || **p != ':')
				return false;
			(*p)++;
			if (!uif_skip_json_value(p, end, depth + 1))
				return false;
			uif_skip_ws(p, end);
			if (*p >= end)
				return false;
			if (**p == ',') {
				(*p)++;
				continue;
			}
			if (**p == '}') {
				(*p)++;
				return true;
			}
			return false;
		}
		return false;
	} else if (c == '[') {
		(*p)++;
		uif_skip_ws(p, end);
		if (*p < end && **p == ']') {
			(*p)++;
			return true;
		}
		while (*p < end) {
			if (!uif_skip_json_value(p, end, depth + 1))
				return false;
			uif_skip_ws(p, end);
			if (*p >= end)
				return false;
			if (**p == ',') {
				(*p)++;
				continue;
			}
			if (**p == ']') {
				(*p)++;
				return true;
			}
			return false;
		}
		return false;
	} else if (end - *p >= 4 && !memcmp(*p, "true", 4)) {
		*p += 4;
		return true;
	} else if (end - *p >= 5 && !memcmp(*p, "false", 5)) {
		*p += 5;
		return true;
	} else if (end - *p >= 4 && !memcmp(*p, "null", 4)) {
		*p += 4;
		return true;
	} else if (c == '-' || (c >= '0' && c <= '9')) {
		if (c == '-') (*p)++;
		if (*p >= end || **p < '0' || **p > '9')
			return false;
		while (*p < end && (**p >= '0' && **p <= '9')) (*p)++;
		if (*p < end && **p == '.') {
			(*p)++;
			while (*p < end && (**p >= '0' && **p <= '9')) (*p)++;
		}
		if (*p < end && (**p == 'e' || **p == 'E')) {
			(*p)++;
			if (*p < end && (**p == '+' || **p == '-')) (*p)++;
			while (*p < end && (**p >= '0' && **p <= '9')) (*p)++;
		}
		return true;
	}
	return false;
}

static bool uif_next_utf8(const uint8_t **p, const uint8_t *end, uint32_t *cp_out)
{
	if (*p >= end)
		return false;
	uint8_t c0 = **p;
	if (c0 < 0x80) {
		*cp_out = c0;
		(*p)++;
		return true;
	}
	if ((c0 & 0xe0) == 0xc0 && end - *p >= 2 && (((*p)[1] & 0xc0) == 0x80)) {
		*cp_out = ((uint32_t)(c0 & 0x1f) << 6) | (uint32_t)((*p)[1] & 0x3f);
		*p += 2;
		return true;
	}
	if ((c0 & 0xf0) == 0xe0 && end - *p >= 3
			&& (((*p)[1] & 0xc0) == 0x80) && (((*p)[2] & 0xc0) == 0x80)) {
		*cp_out = ((uint32_t)(c0 & 0x0f) << 12)
				| ((uint32_t)((*p)[1] & 0x3f) << 6)
				| (uint32_t)((*p)[2] & 0x3f);
		*p += 3;
		return true;
	}
	if ((c0 & 0xf8) == 0xf0 && end - *p >= 4
			&& (((*p)[1] & 0xc0) == 0x80) && (((*p)[2] & 0xc0) == 0x80)
			&& (((*p)[3] & 0xc0) == 0x80)) {
		*cp_out = ((uint32_t)(c0 & 0x07) << 18)
				| ((uint32_t)((*p)[1] & 0x3f) << 12)
				| ((uint32_t)((*p)[2] & 0x3f) << 6)
				| (uint32_t)((*p)[3] & 0x3f);
		*p += 4;
		return true;
	}
	return false;
}

/* Load a UIF (Universal Injection Framework) uif_config.json character_substitution
 * mapping into the custom Shift-JIS render table without mutating base CP932
 * archive/path conversions. */
bool sys4_load_uif_config_mem(const void *data, size_t size)
{
	if (!data || size < 2)
		return false;

	const uint8_t *p = (const uint8_t *)data;
	const uint8_t *end = p + size;
	if (size >= 3 && p[0] == 0xef && p[1] == 0xbb && p[2] == 0xbf)
		p += 3;

	uif_skip_ws(&p, end);
	if (p >= end || *p != '{')
		return false;
	p++;

	bool found_section = false;
	bool enabled = false;
	char *src_chars = NULL;
	char *dst_chars = NULL;
	size_t src_len = 0, dst_len = 0;

	uif_skip_ws(&p, end);
	if (p < end && *p != '}') {
		while (p < end) {
			char *key = NULL;
			if (!uif_parse_json_string(&p, end, &key, NULL))
				goto fail;
			uif_skip_ws(&p, end);
			if (p >= end || *p != ':') {
				free(key);
				goto fail;
			}
			p++;
			if (!strcmp(key, "character_substitution")) {
				free(key);
				uif_skip_ws(&p, end);
				if (p >= end || *p != '{')
					goto fail;
				p++;
				found_section = true;
				uif_skip_ws(&p, end);
				if (p < end && *p != '}') {
					while (p < end) {
						char *subkey = NULL;
						if (!uif_parse_json_string(&p, end, &subkey, NULL))
							goto fail;
						uif_skip_ws(&p, end);
						if (p >= end || *p != ':') {
							free(subkey);
							goto fail;
						}
						p++;
						uif_skip_ws(&p, end);
						if (!strcmp(subkey, "enable")) {
							free(subkey);
							if (end - p >= 4 && !memcmp(p, "true", 4)) {
								enabled = true;
								p += 4;
							} else if (end - p >= 5 && !memcmp(p, "false", 5)) {
								enabled = false;
								p += 5;
							} else {
								goto fail;
							}
						} else if (!strcmp(subkey, "source_characters")) {
							free(subkey);
							free(src_chars);
							src_chars = NULL;
							if (!uif_parse_json_string(&p, end, &src_chars, &src_len))
								goto fail;
						} else if (!strcmp(subkey, "target_characters")) {
							free(subkey);
							free(dst_chars);
							dst_chars = NULL;
							if (!uif_parse_json_string(&p, end, &dst_chars, &dst_len))
								goto fail;
						} else {
							free(subkey);
							if (!uif_skip_json_value(&p, end, 0))
								goto fail;
						}
						uif_skip_ws(&p, end);
						if (p >= end)
							goto fail;
						if (*p == ',') {
							p++;
							continue;
						}
						if (*p == '}') {
							p++;
							break;
						}
						goto fail;
					}
				} else if (p < end && *p == '}') {
					p++;
				}
			} else {
				free(key);
				if (!uif_skip_json_value(&p, end, 0))
					goto fail;
			}
			uif_skip_ws(&p, end);
			if (p >= end)
				goto fail;
			if (*p == ',') {
				p++;
				continue;
			}
			if (*p == '}') {
				p++;
				break;
			}
			goto fail;
		}
	}

	if (!found_section || !enabled || !src_chars || !dst_chars || !src_len || !dst_len)
		goto fail;

	uint16_t *subst = xcalloc(MAI_WORD_TABLE_SIZE, sizeof(uint16_t));
	const uint8_t *sp = (const uint8_t *)src_chars;
	const uint8_t *send = sp + src_len;
	const uint8_t *dp = (const uint8_t *)dst_chars;
	const uint8_t *dend = dp + dst_len;
	size_t pair_count = 0;

	while (sp < send && dp < dend) {
		uint32_t su = 0, du = 0;
		if (!uif_next_utf8(&sp, send, &su) || !uif_next_utf8(&dp, dend, &du)) {
			free(subst);
			goto fail;
		}
		if (su >= 0x80 && su < MAI_WORD_TABLE_SIZE && du > 0 && du < MAI_WORD_TABLE_SIZE) {
			subst[su] = (uint16_t)du;
			pair_count++;
		}
	}
	if (sp != send || dp != dend || !pair_count) {
		free(subst);
		goto fail;
	}

	sys4_reset_mai_word_table();

	size_t mapped_count = 0;
	for (int b1 = 0x81; b1 <= 0xfc; b1++) {
		if (b1 >= 0xa0 && b1 <= 0xdf)
			continue;
		for (int b2 = 0x40; b2 <= 0xfc; b2++) {
			if (b2 == 0x7f)
				continue;
			uint16_t base_u = (uint16_t)s2u[b1 - 0x80][b2 - 0x40];
			uint16_t sjis = (uint16_t)((b1 << 8) | b2);
			if (base_u == 0x30fb && sjis != 0x8145)
				continue;
			if (subst[base_u]) {
				uint16_t dst_u = subst[base_u];
				mai_s2u[b1 - 0x80][b2 - 0x40] = dst_u;
				if (!mai_u2s[dst_u])
					mai_u2s[dst_u] = sjis;
				mapped_count++;
			} else {
				mai_s2u[b1 - 0x80][b2 - 0x40] = base_u;
			}
		}
	}

	free(subst);
	free(src_chars);
	free(dst_chars);

	if (!mapped_count) {
		sys4_reset_mai_word_table();
		return false;
	}

	for (int c = 0xa1; c <= 0xdf; c++) {
		uint16_t u = (uint16_t)(0xff60 + (c - 0xa0));
		if (!mai_u2s[u])
			mai_u2s[u] = (uint16_t)c;
	}
	for (int b1 = 0x81; b1 <= 0xfc; b1++) {
		if (b1 >= 0xa0 && b1 <= 0xdf)
			continue;
		for (int b2 = 0x40; b2 <= 0xfc; b2++) {
			if (b2 == 0x7f)
				continue;
			uint16_t u = mai_s2u[b1 - 0x80][b2 - 0x40];
			if (u >= 0x80 && !mai_u2s[u])
				mai_u2s[u] = (uint16_t)((b1 << 8) | b2);
		}
	}

	sys4_mai_active = true;
	return true;

fail:
	free(src_chars);
	free(dst_chars);
	return false;
}

bool sys4_load_uif_config(const char *path)
{
	if (!path || !*path)
		return false;
	size_t size = 0;
	void *buf = file_read(path, &size);
	if (!buf)
		return false;
	bool ok = sys4_load_uif_config_mem(buf, size);
	free(buf);
	return ok;
}

struct text_dict_entry {
	char *src;
	char *dst;
};

static struct text_dict_entry *text_dict_entries = NULL;
static size_t text_dict_count = 0;

static int text_dict_cmp(const void *a, const void *b)
{
	const struct text_dict_entry *ea = (const struct text_dict_entry *)a;
	const struct text_dict_entry *eb = (const struct text_dict_entry *)b;
	return strcmp(ea->src, eb->src);
}

void sys4_reset_text_dict(void)
{
	for (size_t i = 0; i < text_dict_count; i++) {
		free(text_dict_entries[i].src);
		free(text_dict_entries[i].dst);
	}
	free(text_dict_entries);
	text_dict_entries = NULL;
	text_dict_count = 0;
}

/* Load an XOR-0x49 UI/EX text dictionary (e.g. NightRemasterDict.dat), stored
 * as 3-line records: Shift-JIS source line, GBK target line, separator line. */
bool sys4_load_text_dict_mem(const void *data, size_t size)
{
	sys4_reset_text_dict();
	if (!data || size < 4 || size > 4 * 1024 * 1024)
		return false;

	const uint8_t *raw = (const uint8_t *)data;
	char *dec = xmalloc(size + 1);
	size_t nl_count = 0;
	for (size_t i = 0; i < size; i++) {
		char c = (char)(raw[i] ^ 0x49);
		dec[i] = c;
		if (c == '\n')
			nl_count++;
	}
	dec[size] = '\0';
	if (nl_count < 1) {
		free(dec);
		return false;
	}

	char **lines = xmalloc((nl_count + 1) * sizeof(*lines));
	size_t nr_lines = 0;
	char *p = dec;
	while (true) {
		char *nl = strchr(p, '\n');
		if (nl)
			*nl = '\0';
		size_t len = strlen(p);
		if (len > 0 && p[len - 1] == '\r')
			p[len - 1] = '\0';
		lines[nr_lines++] = p;
		if (!nl)
			break;
		p = nl + 1;
	}

	size_t cap = (nr_lines / 3) + 1;
	struct text_dict_entry *entries = xmalloc(cap * sizeof(*entries));
	size_t count = 0;
	for (size_t i = 0; i + 1 < nr_lines; i += 3) {
		const char *src = lines[i];
		const char *dst = lines[i + 1];
		if (!src[0] || !dst[0] || !strcmp(src, dst))
			continue;
		entries[count].src = xstrdup(src);
		entries[count].dst = xstrdup(dst);
		count++;
	}
	free(lines);
	free(dec);

	if (!count) {
		free(entries);
		return false;
	}

	qsort(entries, count, sizeof(*entries), text_dict_cmp);
	size_t unique = 0;
	for (size_t i = 0; i < count; i++) {
		if (unique > 0 && !strcmp(entries[unique - 1].src, entries[i].src)) {
			free(entries[i].src);
			free(entries[i].dst);
			continue;
		}
		if (unique != i)
			entries[unique] = entries[i];
		unique++;
	}

	text_dict_entries = entries;
	text_dict_count = unique;
	return true;
}

bool sys4_load_text_dict(const char *path)
{
	if (!path || !*path)
		return false;
	size_t size = 0;
	void *buf = file_read(path, &size);
	if (!buf)
		return false;
	bool ok = sys4_load_text_dict_mem(buf, size);
	free(buf);
	return ok;
}

const char *sys4_text_dict_lookup(const char *src)
{
	if (!text_dict_count || !src || !*src)
		return NULL;
	size_t lo = 0, hi = text_dict_count;
	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		int cmp = strcmp(text_dict_entries[mid].src, src);
		if (cmp == 0)
			return text_dict_entries[mid].dst;
		if (cmp < 0)
			lo = mid + 1;
		else
			hi = mid;
	}
	return NULL;
}

char *sys4_text_dict_translate_lines(const char *src)
{
	if (!text_dict_count || !src || !*src)
		return NULL;
	const char *whole = sys4_text_dict_lookup(src);
	if (whole)
		return xstrdup(whole);
	if (!strchr(src, '\n'))
		return NULL;

	// Translate multi-line strings line-by-line only if at least one line matches.
	char *copy = xstrdup(src);
	bool any = false;
	size_t total = 0;
	size_t nlines = 0;
	for (char *ln = copy; ln;) {
		char *nxt = strchr(ln, '\n');
		if (nxt)
			*nxt = '\0';
		const char *rep = sys4_text_dict_lookup(ln);
		if (rep)
			any = true;
		total += strlen(rep ? rep : ln) + (nxt ? 1 : 0);
		nlines++;
		ln = nxt ? nxt + 1 : NULL;
	}
	if (!any) {
		free(copy);
		return NULL;
	}

	char *out = xmalloc(total + 1);
	char *dst = out;
	char *ln = copy;
	for (size_t i = 0; i < nlines; i++) {
		const char *rep = sys4_text_dict_lookup(ln);
		const char *piece = rep ? rep : ln;
		size_t len = strlen(piece);
		memcpy(dst, piece, len);
		dst += len;
		if (i + 1 < nlines)
			*dst++ = '\n';
		ln += strlen(ln) + 1;
	}
	*dst = '\0';
	free(copy);
	return out;
}

void sys4_set_text_gbk(bool enabled)
{
	sys4_text_is_gbk = enabled;
	if (enabled)
		ensure_unicode_to_gbk();
}

void sys4_set_source_utf8(bool enabled)
{
	sys4_source_is_utf8 = enabled;
}

bool sys4_bytes_look_utf8(const uint8_t *s, size_t len)
{
	if (len >= 3 && s[0] == 0xef && s[1] == 0xbb && s[2] == 0xbf)
		return true;
	bool multibyte = false;
	for (size_t i = 0; i < len;) {
		uint8_t b = s[i];
		size_t n = b < 0x80 ? 1 : (b & 0xe0) == 0xc0 ? 2 : (b & 0xf0) == 0xe0 ? 3 : (b & 0xf8) == 0xf0 ? 4 : 0;
		if (!n || i + n > len)
			return false;
		for (size_t j = 1; j < n; j++) {
			if ((s[i + j] & 0xc0) != 0x80)
				return false;
		}
		if (n >= 3)
			multibyte = true;
		i += n;
	}
	return multibyte;
}

// Convert a character-index to a byte-index for a given SJIS string.
int sjis_index(const char *_src, int index)
{
	const uint8_t *src = (uint8_t*)_src;
	int i, c;
	for (i = 0, c = 0; c < index && src[i]; i++, c++) {
		if (SJIS_2BYTE(src[i])) {
			i++;
			// check for invalid sjis data
			if (!src[i])
				return -1;
		}
	}
	return src[i] ? i : -1;
}

static char *cjk_char2unicode_impl(const char *_src, int *dst, bool gbk, bool use_mai)
{
	const uint8_t *src = (const uint8_t*)_src;
	if (*src <= 0x7f) {
		*dst = *src;
		return (char*)src + 1;
	}
	if (!use_mai && gbk) {
		if (*src == 0x80) { *dst = 0x20ac; return (char*)src + 1; }
		if (*src >= 0x81 && *src <= 0xfe && src[1] >= 0x40 && src[1] <= 0xfe) {
			*dst = gbk_lookup(src[0], src[1]);
			if (!*dst) *dst = 0xfffd;
			return (char*)src + 2;
		}
		*dst = 0xfffd;
		return (char*)src + 1;
	}

	if (*src >= 0xa0 && *src <= 0xdf) {
		*dst = 0xff60 + *src - 0xa0;
		return (char*)src + 1;
	}

	// guard against invalid byte sequence
	int c = *(src+1);
	if (c < 0x40 || c == 0x7f || c > 0xfc) {
		*dst = '?';
		return (char*)src + 1;
	}

	if (use_mai) {
		uint16_t u = mai_s2u[*src - 0x80][c - 0x40];
		*dst = u ? u : 0xfffd;
	} else {
		*dst = s2u[*src - 0x80][c - 0x40];
	}
	return (char*)src + 2;
}

char *sjis_char2unicode(const char *_src, int *dst)
{
	return cjk_char2unicode_impl(_src, dst, sys4_text_is_gbk, false);
}

char *mai_sjis_char2unicode(const char *_src, int *dst)
{
	return cjk_char2unicode_impl(_src, dst, sys4_text_is_gbk, sys4_mai_active);
}

static char *cjk2utf_impl(const char *_src, size_t len, bool gbk, bool use_mai) {
	if (!_src)
		return NULL;
	const uint8_t *src = (const uint8_t*)_src;
	if (!len)
		len = strlen(_src);
	const uint8_t *end = src + len;
	uint8_t* dst = xmalloc(len * 3 + 1);
	uint8_t* dstp = dst;

	while (src < end && *src) {
		int c;
		bool is_2b = (!use_mai && gbk)
			? (*src >= 0x81 && *src <= 0xfe)
			: (((*src) & 0xe0) == 0x80 || ((*src) & 0xe0) == 0xe0);
		if (is_2b && !(src + 1 < end && src[1] != '\0')) {
			c = (!use_mai && gbk) ? 0xfffd : '?';
			src++;
		} else {
			src = (const uint8_t*)cjk_char2unicode_impl((const char*)src, &c, gbk, use_mai);
		}

		if (c <= 0x7f) {
			*dstp++ = c;
		} else if (c <= 0x7ff) {
			*dstp++ = 0xc0 | (c >> 6);
			*dstp++ = 0x80 | (c & 0x3f);
		} else {
			*dstp++ = 0xe0 | (c >> 12);
			*dstp++ = 0x80 | ((c >> 6) & 0x3f);
			*dstp++ = 0x80 | (c & 0x3f);
		}
	}
	*dstp = '\0';
	return (char*)dst;
}

char *cjk2utf(const char *_src, size_t len, bool gbk) {
	return cjk2utf_impl(_src, len, gbk, false);
}

char *sjis2utf(const char *_src, size_t len) {
	return cjk2utf_impl(_src, len, sys4_text_is_gbk, false);
}

char *mai_sjis2utf(const char *_src, size_t len) {
	return cjk2utf_impl(_src, len, sys4_text_is_gbk, sys4_mai_active);
}

static void encode_utf8_bmp(int u, uint8_t **dstp)
{
	if (u <= 0x7f) {
		*(*dstp)++ = (uint8_t)u;
	} else if (u <= 0x7ff) {
		*(*dstp)++ = (uint8_t)(0xc0 | (u >> 6));
		*(*dstp)++ = (uint8_t)(0x80 | (u & 0x3f));
	} else {
		*(*dstp)++ = (uint8_t)(0xe0 | (u >> 12));
		*(*dstp)++ = (uint8_t)(0x80 | ((u >> 6) & 0x3f));
		*(*dstp)++ = (uint8_t)(0x80 | (u & 0x3f));
	}
}

/* Decode SJIS (gbk=false) or GBK (gbk=true) to UTF-8 without mutating sys4_text_is_gbk.
 * Writes into caller-supplied `buf` when `len * 3 + 1 <= buf_cap`, otherwise allocates.
 * Returns NULL and sets *valid_out=false if any byte sequence is invalid or unmapped. */
static char *cjk2utf_strict_buf_impl(const char *_src, size_t len, bool gbk, char *buf, size_t buf_cap, bool *valid_out, bool use_mai)
{
	if (valid_out)
		*valid_out = false;
	if (!_src)
		return NULL;
	const uint8_t *src = (const uint8_t *)_src;
	const uint8_t *end = src + len;
	size_t need = len * 3 + 1;
	uint8_t *dst = (buf && need <= buf_cap) ? (uint8_t *)buf : malloc(need);
	if (!dst)
		return NULL;
	uint8_t *dstp = dst;

	while (src < end && *src) {
		uint8_t b0 = *src;
		if (b0 <= 0x7f) {
			*dstp++ = b0;
			src++;
			continue;
		}
		int u = 0;
		if (!gbk) {
			if (b0 >= 0xa1 && b0 <= 0xdf) {
				u = 0xff60 + (b0 - 0xa0);
				src++;
			} else if (((b0 >= 0x81 && b0 <= 0x9f) || (b0 >= 0xe0 && b0 <= 0xfc)) && src + 1 < end && src[1]) {
				uint8_t b1 = src[1];
				size_t r = (size_t)(b0 - 0x80);
				size_t c = (size_t)(b1 - 0x40);
				if (b1 >= 0x40 && b1 != 0x7f && b1 <= 0xfc &&
				    r < sizeof(s2u) / sizeof(s2u[0]) && c < sizeof(s2u_80) / sizeof(s2u_80[0])) {
					u = use_mai ? mai_s2u[r][c] : s2u[r][c];
					if (!use_mai && u == 0x30fb && (b0 != 0x81 || b1 != 0x45))
						u = 0;
				}
				src += 2;
			}
		} else {
			if (b0 >= 0x81 && b0 <= 0xfe && src + 1 < end && src[1]) {
				uint8_t b1 = src[1];
				size_t r = (size_t)(b0 - 0x81);
				size_t c = (size_t)(b1 - 0x40);
				if (b1 >= 0x40 && r < sizeof(gbk_to_unicode) / sizeof(gbk_to_unicode[0]) &&
				    c < sizeof(gbk_to_unicode[0]) / sizeof(gbk_to_unicode[0][0]))
					u = gbk_lookup(b0, b1);
				src += 2;
			}
		}
		if (u <= 0) {
			if (dst != (uint8_t *)buf)
				free(dst);
			return NULL;
		}
		encode_utf8_bmp(u, &dstp);
	}
	*dstp = '\0';
	if (valid_out)
		*valid_out = true;
	return (char *)dst;
}

char *cjk2utf_strict_buf(const char *_src, size_t len, bool gbk, char *buf, size_t buf_cap, bool *valid_out)
{
	return cjk2utf_strict_buf_impl(_src, len, gbk, buf, buf_cap, valid_out, false);
}

/* Decode SJIS (gbk=false) or GBK (gbk=true) to UTF-8 without mutating sys4_text_is_gbk.
 * Returns NULL and sets *valid_out=false if any byte sequence is invalid or unmapped. */
char *cjk2utf_strict(const char *_src, bool gbk, bool *valid_out)
{
	return cjk2utf_strict_buf(_src, _src ? strlen(_src) : 0, gbk, NULL, 0, valid_out);
}

char *mai_cjk2utf_strict(const char *_src, bool *valid_out)
{
	if (!sys4_mai_active)
		return cjk2utf_strict(_src, sys4_text_is_gbk, valid_out);
	return cjk2utf_strict_buf_impl(_src, _src ? strlen(_src) : 0, false, NULL, 0, valid_out, true);
}

static int cjk_encode_codepoint_impl(int u, bool gbk, bool use_mai)
{
	if (u <= 0x7f)
		return u >= 0 ? u : 0;
	if (!gbk && u > 0xff60 && u <= 0xff9f)
		return u - 0xff60 + 0xa0;
	if (gbk && u == 0x20ac)
		return 0x80;
	if (u >= 65536)
		return 0;
	if (use_mai && !gbk)
		return mai_u2s[u];
	if (gbk) {
		ensure_unicode_to_gbk();
		return unicode_to_gbk[u];
	}
	ensure_unicode_to_sjis();
	return unicode_to_sjis_tbl[u];
}

int cjk_encode_codepoint(int u, bool gbk)
{
	return cjk_encode_codepoint_impl(u, gbk, false);
}

static int sjis_display_pair(const uint8_t *s)
{
	if (!((*s >= 0x81 && *s <= 0x9f) || (*s >= 0xe0 && *s <= 0xfc))
			|| s[1] < 0x40 || s[1] > 0xfc || s[1] == 0x7f)
		return 0;
	int u = s2u[*s - 0x80][s[1] - 0x40];
	if (u == 0x30fb && (s[0] != 0x81 || s[1] != 0x45))
		return 0;
	return u;
}

static bool gbk_display_pair(const uint8_t *s)
{
	return s[0] >= 0x81 && s[0] <= 0xfe && s[1] >= 0x40 && s[1] <= 0xfe && s[1] != 0x7f;
}

static bool gb2312_display_pair(const uint8_t *s)
{
	return s[0] >= 0xa1 && s[0] <= 0xf7 && s[1] >= 0xa1 && s[1] <= 0xfe
		&& gbk_lookup(s[0], s[1]);
}

static int gbk_display_code(int u)
{
	int gbk = cjk_encode_codepoint(u, true);
	return gbk > 0xff && gbk_lookup(gbk >> 8, gbk & 0xff) == u ? gbk : 0;
}

static bool sjis_display_run_to_gbk(char *text, size_t len)
{
	for (size_t i = 0; i < len; i += 2) {
		int gbk = gbk_display_code(sjis_display_pair((const uint8_t *)text + i));
		if (!gbk)
			return false;
		text[i] = gbk >> 8;
		text[i + 1] = gbk & 0xff;
	}
	return true;
}

static char *sjis_display_question_to_gbk(const char *text)
{
	size_t len = strlen(text);
	const uint8_t *s = (const uint8_t *)text;
	if (len < 12 || len % 2 || sjis_display_pair(s + len - 2) != 0xff1f)
		return NULL;

	// Require a complete Japanese question with a kana ending, not just dual-valid bytes.
	size_t trailing_hiragana = 0;
	for (size_t i = 0; i < len - 2; i += 2) {
		int u = sjis_display_pair(s + i);
		if (!u || gb2312_display_pair(s + i))
			return NULL;
		trailing_hiragana = u >= 0x3041 && u <= 0x3096 ? trailing_hiragana + 1 : 0;
	}
	if (trailing_hiragana < 4)
		return NULL;

	char *display = xstrdup(text);
	if (sjis_display_run_to_gbk(display, len))
		return display;
	free(display);
	return NULL;
}

static bool display_number_char(int u)
{
	return (u >= 0xff10 && u <= 0xff19) || u == 0x3000
		|| u == 0xff0b || u == 0xff0d || u == 0xff0e || u == 0xff0f || u == 0xff1a;
}

char *cjk_mixed_display_to_gbk(const char *text, bool allow_ambiguous)
{
	char *out = sjis_display_question_to_gbk(text);
	if (out)
		return out;

	const uint8_t *start = (const uint8_t *)text;
	const uint8_t *s = start;
	while (*s) {
		if (!gbk_display_pair(s)) {
			s++;
			continue;
		}
		const uint8_t *end = s;
		bool token_has_gbk = false, token_has_gbk_text = false, token_is_numeric = true;
		bool has_gbk_digit = false, has_sjis_separator = false;
		while (gbk_display_pair(end)) {
			int sjis = sjis_display_pair(end);
			int gbk = gbk_lookup(end[0], end[1]);
			token_is_numeric &= display_number_char(sjis) || display_number_char(gbk);
			has_gbk_digit |= gbk >= 0xff10 && gbk <= 0xff19;
			has_sjis_separator |= sjis == 0xff0f || sjis == 0xff1a;
			if (!sjis && gbk) {
				token_has_gbk = true;
				token_has_gbk_text |= !(end[0] == 0xa1 && end[1] == 0xa1)
					&& !(end[0] == 0xa3 && end[1] >= 0xb0 && end[1] <= 0xb9);
			}
			end += 2;
		}
		if (!allow_ambiguous && !(token_is_numeric && has_gbk_digit && has_sjis_separator)) {
			s = end;
			continue;
		}
		while (s < end) {
			const uint8_t *run = s;
			size_t extended = 0, common = 0;
			bool has_sjis_only = false, has_kana = false, has_kanji = false;
			int u;
			while (s < end && (u = sjis_display_pair(s)) > 0) {
				has_sjis_only |= !gbk_lookup(s[0], s[1]);
				has_kana |= (u >= 0x3041 && u <= 0x3096) || (u >= 0x30a1 && u <= 0x30fa);
				has_kanji |= u >= 0x4e00 && u <= 0x9fff;
				if (s[0] >= 0xa1 && s[0] <= 0xf7 && s[1] >= 0xa1)
					common++;
				else
					extended++;
				s += 2;
			}
			if (run == s) {
				s += 2;
				continue;
			}
			// Prefer GBK inside a GB2312 run, and require text evidence next to GBK.
			if (!token_is_numeric && !has_sjis_only
					&& ((common && (!has_kana || extended / 3 < common))
						|| token_has_gbk_text
						|| (token_has_gbk && (extended < 2 || (!has_kana && !has_kanji)))))
				continue;
			for (const uint8_t *p = run; p < s; p += 2) {
				int code = sjis_display_pair(p);
				if (!allow_ambiguous && code != 0xff0f && code != 0xff1a)
					continue;
				int gbk = gbk_display_code(code);
				if (!gbk || ((p[0] << 8) | p[1]) == gbk)
					continue;
				if (!out)
					out = xstrdup(text);
				out[p - start] = gbk >> 8;
				out[p - start + 1] = gbk & 0xff;
			}
		}
	}
	return out;
}

static bool gbk_display_pair_bounded(const uint8_t *s, const uint8_t *end)
{
	return s + 1 < end && gbk_display_pair(s);
}

struct cjk_dialog_run_stats {
	size_t common;
	size_t extended;
	bool has_sjis_only;
	bool has_sjis_punct;
	bool all_sjis_punct_or_space;
};

static bool cjk_dialog_is_sjis_punct_pair(const uint8_t *p)
{
	if (p[0] != 0x81)
		return false;
	switch (p[1]) {
	case 0x41: // 、
	case 0x42: // 。
	case 0x45: // ・
	case 0x46: // ：
	case 0x47: // ；
	case 0x48: // ？
	case 0x49: // ！
	case 0x63: // …
	case 0x69: // （
	case 0x6a: // ）
	case 0x6d: // ［
	case 0x6e: // ］
	case 0x71: // 〈
	case 0x72: // 〉
	case 0x73: // 《
	case 0x74: // 》
	case 0x75: // 「
	case 0x76: // 」
	case 0x77: // 『
	case 0x78: // 』
	case 0x79: // 【
	case 0x7a: // 】
		return true;
	default:
		return false;
	}
}

static bool cjk_dialog_is_sjis_hw_kana_run(const uint8_t *s, const uint8_t *end)
{
	size_t len = (size_t)(end - s);
	if (!len)
		return false;
	// Odd-length 0xA1..0xDF runs cannot be valid GBK double-byte sequences.
	if (len & 1u)
		return true;
	// Any pair unmapped in GBK cannot be valid GBK text.
	for (size_t i = 0; i < len; i += 2) {
		if (!gbk_lookup(s[i], s[i + 1]))
			return true;
	}
	// Even-length 0xA1..0xDF runs overlap GB2312 (e.g. 中文/系统); only match
	// explicit CP932 half-width katakana system words inside a CP932 dialog.
	static const struct {
		const uint8_t bytes[6];
		uint8_t len;
	} hw_words[] = {
		{ { 0xcc, 0xa7, 0xb2, 0xd9 }, 4 },                   // ﾌｧｲﾙ
		{ { 0xbe, 0xb0, 0xcc, 0xde }, 4 },                   // ｾｰﾌﾞ
		{ { 0xdb, 0xb0, 0xc4, 0xde }, 4 },                   // ﾛｰﾄﾞ
		{ { 0xc3, 0xde, 0xb0, 0xc0 }, 4 },                   // ﾃﾞｰﾀ
		{ { 0xbc, 0xbd, 0xc3, 0xd1 }, 4 },                   // ｼｽﾃﾑ
		{ { 0xd2, 0xaf, 0xbe, 0xb0, 0xbc, 0xde }, 6 },       // ﾒｯｾｰｼﾞ
		{ { 0xc0, 0xb2, 0xc4, 0xd9 }, 4 },                   // ﾀｲﾄﾙ
	};
	for (size_t i = 0; i < sizeof(hw_words) / sizeof(hw_words[0]); i++) {
		size_t wlen = hw_words[i].len;
		if (wlen > len)
			continue;
		for (size_t off = 0; off <= len - wlen; off++) {
			if (!memcmp(s + off, hw_words[i].bytes, wlen))
				return true;
		}
	}
	return false;
}

static void cjk_dialog_scan_sjis_run(const uint8_t *run, const uint8_t *end,
		struct cjk_dialog_run_stats *st)
{
	memset(st, 0, sizeof(*st));
	st->all_sjis_punct_or_space = (run < end);
	for (const uint8_t *p = run; p < end; p += 2) {
		if (!gbk_lookup(p[0], p[1]))
			st->has_sjis_only = true;
		if (p[0] >= 0xa1 && p[0] <= 0xf7 && p[1] >= 0xa1)
			st->common++;
		else
			st->extended++;
		if (cjk_dialog_is_sjis_punct_pair(p))
			st->has_sjis_punct = true;
		else if (!(p[0] == 0x81 && p[1] == 0x40))
			st->all_sjis_punct_or_space = false;
	}
}

/* Recognized CP932 System 4 dialog keywords and sentence endings.
 * Matched strictly on 2-byte pair boundaries inside valid Shift-JIS runs so
 * arbitrary dual-valid GBK text (e.g. 傑傑傑傑, 偽鈔偵測機) is never overridden. */
static bool cjk_dialog_run_has_sjis_phrase(const uint8_t *run, const uint8_t *end)
{
	static const struct {
		const uint8_t bytes[10];
		uint8_t len;
	} phrases[] = {
		{ { 0x83, 0x5a, 0x81, 0x5b, 0x83, 0x75 }, 6 },                         // セーブ
		{ { 0x83, 0x8d, 0x81, 0x5b, 0x83, 0x68 }, 6 },                         // ロード
		{ { 0x83, 0x66, 0x81, 0x5b, 0x83, 0x5e }, 6 },                         // データ
		{ { 0x83, 0x47, 0x83, 0x89, 0x81, 0x5b }, 6 },                         // エラー
		{ { 0x83, 0x74, 0x83, 0x40, 0x83, 0x43, 0x83, 0x8b }, 8 },             // ファイル
		{ { 0x83, 0x56, 0x83, 0x58, 0x83, 0x65, 0x83, 0x80 }, 8 },             // システム
		{ { 0x83, 0x5e, 0x83, 0x43, 0x83, 0x67, 0x83, 0x8b }, 8 },             // タイトル
		{ { 0x83, 0x81, 0x83, 0x62, 0x83, 0x5a, 0x81, 0x5b, 0x83, 0x57 }, 10 },// メッセージ
		{ { 0x83, 0x81, 0x83, 0x82, 0x83, 0x8a }, 6 },                         // メモリ
		{ { 0x82, 0xb5, 0x82, 0xdc, 0x82, 0xb5, 0x82, 0xbd }, 8 },             // しました
		{ { 0x82, 0xdc, 0x82, 0xb7, 0x82, 0xa9, 0x81, 0x48 }, 8 },             // ますか？
		{ { 0x82, 0xc5, 0x82, 0xb7, 0x82, 0xa9, 0x81, 0x48 }, 8 },             // ですか？
		{ { 0x82, 0xb7, 0x82, 0xe9, 0x81, 0x48 }, 6 },                         // する？
		{ { 0x82, 0xdc, 0x82, 0xb9, 0x82, 0xf1 }, 6 },                         // ません
		{ { 0x82, 0xc4, 0x82, 0xa2, 0x82, 0xdc, 0x82, 0xb7 }, 8 },             // ています
		{ { 0x82, 0xc9, 0x8e, 0xb8, 0x94, 0x73 }, 6 },                         // に失敗
		{ { 0x82, 0xad, 0x82, 0xbe, 0x82, 0xb3, 0x82, 0xa2 }, 8 },             // ください
		{ { 0x82, 0xdc, 0x82, 0xb5, 0x82, 0xbd, 0x81, 0x42 }, 8 },             // ました。
	};
	size_t run_len = (size_t)(end - run);
	for (size_t i = 0; i < sizeof(phrases) / sizeof(phrases[0]); i++) {
		size_t plen = phrases[i].len;
		if (plen > run_len)
			continue;
		for (size_t off = 0; off <= run_len - plen; off += 2) {
			if (!memcmp(run + off, phrases[i].bytes, plen))
				return true;
		}
	}
	return false;
}

static bool cjk_dialog_run_is_strong_sjis(const uint8_t *run, const uint8_t *end,
		const struct cjk_dialog_run_stats *st)
{
	if (st->has_sjis_only)
		return true;
	if (st->common && st->extended / 3 < st->common)
		return false;
	return cjk_dialog_run_has_sjis_phrase(run, end);
}

static bool cjk_dialog_has_sjis_evidence(const uint8_t *src, const uint8_t *end)
{
	const uint8_t *s = src;
	while (s < end) {
		if (!gbk_display_pair_bounded(s, end)) {
			s++;
			continue;
		}
		const uint8_t *tok_end = s;
		bool token_has_gbk_text = false;
		while (gbk_display_pair_bounded(tok_end, end)) {
			int sjis = sjis_display_pair(tok_end);
			int gbk = gbk_lookup(tok_end[0], tok_end[1]);
			if (!sjis && gbk) {
				token_has_gbk_text |= !(tok_end[0] == 0xa1 && tok_end[1] == 0xa1)
					&& !(tok_end[0] == 0xa3 && tok_end[1] >= 0xb0 && tok_end[1] <= 0xb9);
			}
			tok_end += 2;
		}
		while (s < tok_end) {
			const uint8_t *run = s;
			while (s < tok_end && sjis_display_pair(s) > 0)
				s += 2;
			if (run == s) {
				s += 2;
				continue;
			}
			struct cjk_dialog_run_stats st;
			cjk_dialog_scan_sjis_run(run, s, &st);
			if (st.has_sjis_only || (!token_has_gbk_text && cjk_dialog_run_is_strong_sjis(run, s, &st)))
				return true;
		}
	}
	return false;
}

char *cjk_dialog_text_to_utf8(const char *_src, size_t len)
{
	if (!_src)
		return NULL;
	if (!len) {
		len = strlen(_src);
	} else {
		const char *nul = memchr(_src, '\0', len);
		if (nul)
			len = (size_t)(nul - _src);
	}
	if (!len || !sys4_text_is_gbk || sys4_mai_active)
		return mai_sjis2utf(_src, len);

	const uint8_t *src = (const uint8_t *)_src;
	const uint8_t *end = src + len;
	if (!cjk_dialog_has_sjis_evidence(src, end))
		return mai_sjis2utf(_src, len);

	uint8_t *dst = xmalloc(len * 3 + 1);
	uint8_t *dstp = dst;
	const uint8_t *s = src;
	while (s < end) {
		if (*s >= 0xa1 && *s <= 0xdf) {
			const uint8_t *na_end = s;
			bool all_hw = true;
			while (na_end < end && *na_end >= 0x80) {
				if (*na_end < 0xa1 || *na_end > 0xdf)
					all_hw = false;
				na_end++;
			}
			bool crosses_gbk_pair = ((size_t)(na_end - s) & 1u) != 0
				&& na_end < end
				&& gbk_display_pair_bounded(na_end - 1, end)
				&& gbk_lookup(na_end[-1], na_end[0]) > 0;
			if (all_hw && !crosses_gbk_pair && cjk_dialog_is_sjis_hw_kana_run(s, na_end)) {
				for (const uint8_t *p = s; p < na_end; p++)
					encode_utf8_bmp(0xff60 + (*p - 0xa0), &dstp);
				s = na_end;
				continue;
			}
		}
		if (!gbk_display_pair_bounded(s, end)) {
			if (*s <= 0x7f)
				*dstp++ = *s;
			else if (*s == 0x80)
				encode_utf8_bmp(0x20ac, &dstp);
			else
				encode_utf8_bmp(0xfffd, &dstp);
			s++;
			continue;
		}
		const uint8_t *tok_end = s;
		bool token_has_gbk_text = false;
		while (gbk_display_pair_bounded(tok_end, end)) {
			int sjis = sjis_display_pair(tok_end);
			int gbk = gbk_lookup(tok_end[0], tok_end[1]);
			if (!sjis && gbk) {
				token_has_gbk_text |= !(tok_end[0] == 0xa1 && tok_end[1] == 0xa1)
					&& !(tok_end[0] == 0xa3 && tok_end[1] >= 0xb0 && tok_end[1] <= 0xb9);
			}
			tok_end += 2;
		}
		while (s < tok_end) {
			const uint8_t *run = s;
			while (s < tok_end && sjis_display_pair(s) > 0)
				s += 2;
			if (run == s) {
				int g = gbk_lookup(s[0], s[1]);
				encode_utf8_bmp(g ? g : 0xfffd, &dstp);
				s += 2;
				continue;
			}
			struct cjk_dialog_run_stats st;
			cjk_dialog_scan_sjis_run(run, s, &st);
			bool decode_as_sjis = st.has_sjis_only
				|| (!token_has_gbk_text
					&& (cjk_dialog_run_is_strong_sjis(run, s, &st)
						|| (st.all_sjis_punct_or_space && st.has_sjis_punct)));
			for (const uint8_t *p = run; p < s; p += 2) {
				int code = decode_as_sjis ? sjis_display_pair(p) : gbk_lookup(p[0], p[1]);
				encode_utf8_bmp(code ? code : 0xfffd, &dstp);
			}
		}
	}
	*dstp = '\0';
	return (char *)dst;
}

struct string *cjk_display_log_line(struct string *line)
{
	const uint8_t *text = (const uint8_t *)line->text;
	if (!sys4_text_is_gbk || sys4_source_is_utf8 || line->size < 4
			|| text[0] != 0x81 || text[1] != 0x79)
		return string_ref(line);

	// The legacy Shift-JIS 【%s】 wrapper may enclose either a GBK name or a Shift-JIS name.
	const uint8_t *end = text + line->size - 2;
	if (end[0] != 0x81 || end[1] != 0x7a)
		return string_ref(line);

	// Only the wrapper gives context to interpret an otherwise ambiguous kanji name.
	const uint8_t *name = text + 2;
	size_t name_len = end - name;
	bool sjis_name = name_len >= 4 && name_len % 2 == 0;
	for (size_t i = 0; sjis_name && i < name_len; i += 2) {
		int u = sjis_display_pair(name + i);
		sjis_name = u >= 0x4e00 && u <= 0x9fff && !gb2312_display_pair(name + i)
			&& gbk_display_code(u);
	}
	if (sjis_name) {
		struct string *display = string_dup(line);
		if (sjis_display_run_to_gbk(display->text, line->size))
			return display;
		free_string(display);
	}
	for (const uint8_t *p = name; p < end;) {
		if (*p >= 0x20 && *p <= 0x80) {
			p++;
		} else if (end - p >= 2 && gbk_display_pair(p) && gbk_lookup(p[0], p[1])) {
			p += 2;
		} else {
			return string_ref(line);
		}
	}

	struct string *display = string_dup(line);
	display->text[0] = (char)0xa1;
	display->text[1] = (char)0xbe;
	display->text[line->size - 2] = (char)0xa1;
	display->text[line->size - 1] = (char)0xbf;
	return display;
}

static char *utf2cjk_impl(const char *_src, size_t len, bool gbk, bool use_mai) {
	if (!_src)
		return NULL;
	const uint8_t *src = (const uint8_t*)_src;
	if (!len)
		len = strlen(_src);
	const uint8_t *end = src + len;
	uint8_t* dst = xmalloc(len * 2 + 1);
	uint8_t* dstp = dst;

	while (src < end && *src) {
		if (*src <= 0x7f) {
			*dstp++ = *src++;
			continue;
		}

		int u;
		if ((*src & 0xe0) == 0xc0) {
			if (src + 1 >= end || (src[1] & 0xc0) != 0x80) {
				*dstp++ = '?';
				src++;
				continue;
			}
			u = ((src[0] & 0x1f) << 6) | (src[1] & 0x3f);
			src += 2;
		} else if ((*src & 0xf0) == 0xe0) {
			if (src + 2 >= end || (src[1] & 0xc0) != 0x80 || (src[2] & 0xc0) != 0x80) {
				*dstp++ = '?';
				src++;
				while (src < end && (*src & 0xc0) == 0x80)
					src++;
				continue;
			}
			u = ((src[0] & 0x0f) << 12) | ((src[1] & 0x3f) << 6) | (src[2] & 0x3f);
			src += 3;
		} else {
			*dstp++ = '?';
			src++;
			while (src < end && (*src & 0xc0) == 0x80)
				src++;
			continue;
		}

		int c = cjk_encode_codepoint_impl(u, gbk, use_mai);
		if (c > 0xff) {
			*dstp++ = (uint8_t)(c >> 8);
			*dstp++ = (uint8_t)(c & 0xff);
		} else if (c) {
			*dstp++ = (uint8_t)(c & 0xff);
		} else {
			*dstp++ = '?';
		}
	}
	*dstp = '\0';
	return (char*)dst;
}

char *utf2cjk(const char *_src, size_t len, bool gbk) {
	return utf2cjk_impl(_src, len, gbk, false);
}

static bool bytes_are_valid_utf8_non_ascii(const uint8_t *s, size_t len)
{
	bool has_non_ascii = false;
	for (size_t i = 0; i < len; ) {
		uint8_t b = s[i];
		if (b < 0x80) {
			i++;
			continue;
		}
		size_t n = (b >= 0xc2 && b <= 0xdf) ? 2 : (b & 0xf0) == 0xe0 ? 3 : (b & 0xf8) == 0xf0 ? 4 : 0;
		if (!n || i + n > len)
			return false;
		for (size_t j = 1; j < n; j++) {
			if ((s[i + j] & 0xc0) != 0x80)
				return false;
		}
		has_non_ascii = true;
		i += n;
	}
	return has_non_ascii;
}

/* Game data from UTF-8 releases is converted to GBK on load so it runs on the GBK text path.
 * Leave non-UTF-8 legacy strings untouched so mixed repacks (e.g. UTF-8 AIN + SJIS EX/AFA) do not turn into '?'. */
char *sys4_source_strdup(const char *src)
{
	if (!sys4_source_is_utf8 || !src || !bytes_are_valid_utf8_non_ascii((const uint8_t *)src, strlen(src)))
		return strdup(src);
	return utf2cjk_impl(src, strlen(src), sys4_text_is_gbk, false);
}

struct string *sys4_source_make_string(const char *src, size_t len)
{
	if (!sys4_source_is_utf8 || !len || !bytes_are_valid_utf8_non_ascii((const uint8_t *)src, len))
		return make_string(src, len);
	char *conv = utf2cjk_impl(src, len, sys4_text_is_gbk, false);
	struct string *s = make_string(conv, strlen(conv));
	free(conv);
	return s;
}

char *utf2sjis(const char *_src, size_t len) {
	return utf2cjk_impl(_src, len, sys4_text_is_gbk, false);
}

char *mai_utf2sjis(const char *_src, size_t len) {
	if (!sys4_mai_active)
		return utf2sjis(_src, len);
	return utf2cjk_impl(_src, len, false, true);
}

static bool seg_is_strict_sjis_non_gb2312(const uint8_t *src, size_t len)
{
	if (!src || !len)
		return false;
	size_t cjk_pairs = 0;
	size_t non_gb2312 = 0;
	for (size_t i = 0; i < len && src[i];) {
		uint8_t b0 = src[i];
		if (b0 <= 0x7f) {
			i++;
			continue;
		}
		// Restrict to standard CP932 double-byte lead ranges (0x81..0x9F, 0xE0..0xEF);
		// rejects halfwidth kana (0xA1..0xDF) and user/extended ranges (>= 0xF0).
		if (!((b0 >= 0x81 && b0 <= 0x9f) || (b0 >= 0xe0 && b0 <= 0xef)))
			return false;
		if (i + 1 >= len || !src[i + 1])
			return false;
		uint8_t b1 = src[i + 1];
		if (b1 < 0x40 || b1 == 0x7f || b1 > 0xfc)
			return false;
		size_t r = (size_t)(b0 - 0x80);
		size_t c = (size_t)(b1 - 0x40);
		if (r >= sizeof(s2u) / sizeof(s2u[0]) || c >= sizeof(s2u_80) / sizeof(s2u_80[0]))
			return false;
		int u = s2u[r][c];
		if (u <= 0 || (u == 0x30fb && (b0 != 0x81 || b1 != 0x45)))
			return false;
		cjk_pairs++;
		if (b0 < 0xa1 || b1 < 0xa1)
			non_gb2312++;
		i += 2;
	}
	return cjk_pairs > 0 && non_gb2312 == cjk_pairs;
}

static char *decode_cjk_raw_seg(const uint8_t *seg, size_t seg_len, bool use_sjis)
{
	bool ok = false;
	char *u = cjk2utf_strict_buf((const char *)seg, seg_len, !use_sjis, NULL, 0, &ok);
	if (u && ok)
		return u;
	free(u);
	char *tmp = xmalloc(seg_len + 1);
	memcpy(tmp, seg, seg_len);
	tmp[seg_len] = '\0';
	char *fallback = sjis2utf(tmp, seg_len);
	free(tmp);
	return fallback;
}

/* Decode VM/INI path string when sys4_text_is_gbk is enabled:
 * per path segment, if raw bytes are valid extended-SJIS (never GB2312 and no halfwidth kana),
 * decode as CP932 instead of GBK so filenames like 戦国ランス_Sys.ASD / ランス・クエスト.ain
 * do not turn into 愴崙儔儞僗_Sys.ASD / 儔儞僗丒僋僄僗僩.ain. */
char *sjis_filename2utf(const char *_src, size_t len)
{
	if (!_src)
		return NULL;
	if (!len)
		len = strlen(_src);
	if (!sys4_text_is_gbk)
		return sjis2utf(_src, len);

	const uint8_t *src = (const uint8_t *)_src;
	size_t out_cap = len * 3 + 1;
	char *out = xmalloc(out_cap);
	size_t out_len = 0;

	size_t i = 0;
	while (i < len && src[i]) {
		size_t seg_start = i;
		while (i < len && src[i] && src[i] != '/' && src[i] != '\\') {
			if ((sys4_text_is_gbk ? (src[i] >= 0x81 && src[i] <= 0xfe) : (((src[i]) & 0xe0) == 0x80 || ((src[i]) & 0xe0) == 0xe0)) &&
			    i + 1 < len && src[i + 1]) {
				i += 2;
			} else {
				i++;
			}
		}
		size_t seg_len = i - seg_start;
		if (seg_len > 0) {
			bool use_sjis = seg_is_strict_sjis_non_gb2312(src + seg_start, seg_len);
			char *seg_utf = decode_cjk_raw_seg(src + seg_start, seg_len, use_sjis);
			size_t slen = strlen(seg_utf);
			if (out_len + slen + 2 > out_cap) {
				out_cap = out_len + slen + 64;
				out = xrealloc(out, out_cap);
			}
			memcpy(out + out_len, seg_utf, slen);
			out_len += slen;
			free(seg_utf);
		}
		if (i < len && (src[i] == '/' || src[i] == '\\')) {
			if (out_len + 2 > out_cap) {
				out_cap += 64;
				out = xrealloc(out, out_cap);
			}
			out[out_len++] = (char)src[i++];
		}
	}
	out[out_len] = '\0';
	return out;
}

static bool encode_utf8_to_gbk_strict(const char *seg, size_t seg_len, uint8_t **out_buf, size_t *out_len)
{
	uint8_t *dst = xmalloc(seg_len + 1);
	size_t pos = 0;
	const uint8_t *p = (const uint8_t *)seg;
	const uint8_t *end = p + seg_len;
	while (p < end && *p) {
		if (*p <= 0x7f) {
			dst[pos++] = *p++;
			continue;
		}
		int u = 0;
		if (*p <= 0xdf && p + 1 < end) {
			if ((*p & 0xe0) != 0xc0 || (p[1] & 0xc0) != 0x80) { free(dst); return false; }
			u = ((p[0] & 0x1f) << 6) | (p[1] & 0x3f);
			p += 2;
		} else if (*p <= 0xef && p + 2 < end) {
			if ((*p & 0xf0) != 0xe0 || (p[1] & 0xc0) != 0x80 || (p[2] & 0xc0) != 0x80) { free(dst); return false; }
			u = ((p[0] & 0x0f) << 12) | ((p[1] & 0x3f) << 6) | (p[2] & 0x3f);
			p += 3;
		} else {
			free(dst);
			return false;
		}
		int c = cjk_encode_codepoint(u, true);
		if (c <= 0xff) {
			free(dst);
			return false;
		}
		dst[pos++] = (uint8_t)(c >> 8);
		dst[pos++] = (uint8_t)(c & 0xff);
	}
	dst[pos] = '\0';
	*out_buf = dst;
	*out_len = pos;
	return true;
}

static void utf8_jp_filename_quality(const char *seg, size_t seg_len, int *score_out, size_t *mojibake_out)
{
	int score = 0;
	size_t mojibake = 0;
	const uint8_t *p = (const uint8_t *)seg;
	const uint8_t *end = p + seg_len;
	while (p < end && *p) {
		if (*p <= 0x7f) {
			p++;
			continue;
		}
		int u = 0;
		if ((*p & 0xe0) == 0xc0 && p + 1 < end && (p[1] & 0xc0) == 0x80) {
			u = ((p[0] & 0x1f) << 6) | (p[1] & 0x3f);
			p += 2;
		} else if ((*p & 0xf0) == 0xe0 && p + 2 < end && (p[1] & 0xc0) == 0x80 && (p[2] & 0xc0) == 0x80) {
			u = ((p[0] & 0x0f) << 12) | ((p[1] & 0x3f) << 6) | (p[2] & 0x3f);
			p += 3;
		} else {
			p++;
			continue;
		}
		if ((u >= 0x3041 && u <= 0x3096) || (u >= 0x30a1 && u <= 0x30fa) ||
		    (u >= 0x3000 && u <= 0x303f) || u == 0x30fb || u == 0x30fc ||
		    (u >= 0xff01 && u <= 0xff5e)) {
			score += 3;
			continue;
		}
		int sj = cjk_encode_codepoint(u, false);
		if ((sj >= 0x8140 && sj <= 0x84be) || (sj >= 0x889f && sj <= 0x9872)) {
			score += 2;
		} else if (sj >= 0x989f && sj <= 0xeaa4) {
			score -= 1;
			mojibake++;
		} else {
			score -= 3;
			mojibake++;
		}
	}
	*score_out = score;
	*mojibake_out = mojibake;
}

/* Repair a UTF-8 path/filename that contains CP932-as-GBK mojibake
 * (e.g. "愴崙儔儞僗GA.ald" -> "戦国ランスGA.ald"). Returns newly allocated string if repaired, else NULL. */
char *cjk_repair_sjis_as_gbk_utf8(const char *utf)
{
	if (!utf)
		return NULL;
	bool has_non_ascii = false;
	for (const uint8_t *p = (const uint8_t *)utf; *p; p++) {
		if (*p > 0x7f) {
			has_non_ascii = true;
			break;
		}
	}
	if (!has_non_ascii)
		return NULL;

	size_t len = strlen(utf);
	size_t out_cap = len * 2 + 1;
	char *out = xmalloc(out_cap);
	size_t out_len = 0;
	bool changed = false;

	size_t i = 0;
	while (i < len) {
		size_t seg_start = i;
		while (i < len && utf[i] != '/' && utf[i] != '\\')
			i++;
		size_t seg_len = i - seg_start;
		if (seg_len > 0) {
			bool seg_repaired = false;
			uint8_t *gbk_bytes = NULL;
			size_t gbk_len = 0;
			if (encode_utf8_to_gbk_strict(utf + seg_start, seg_len, &gbk_bytes, &gbk_len) &&
			    seg_is_strict_sjis_non_gb2312(gbk_bytes, gbk_len)) {
				bool ok = false;
				char *sjis_utf = cjk2utf_strict_buf((const char *)gbk_bytes, gbk_len, false, NULL, 0, &ok);
				if (sjis_utf && ok) {
					size_t rlen = strlen(sjis_utf);
					int orig_score = 0, rep_score = 0;
					size_t orig_mojibake = 0, rep_mojibake = 0;
					utf8_jp_filename_quality(utf + seg_start, seg_len, &orig_score, &orig_mojibake);
					utf8_jp_filename_quality(sjis_utf, rlen, &rep_score, &rep_mojibake);
					if (orig_mojibake > 0 && rep_score > orig_score && rep_mojibake < orig_mojibake) {
						if (out_len + rlen + 2 > out_cap) {
							out_cap = out_len + rlen + 64;
							out = xrealloc(out, out_cap);
						}
						memcpy(out + out_len, sjis_utf, rlen);
						out_len += rlen;
						changed = true;
						seg_repaired = true;
					}
				}
				free(sjis_utf);
			}
			free(gbk_bytes);
			if (!seg_repaired) {
				if (out_len + seg_len + 2 > out_cap) {
					out_cap = out_len + seg_len + 64;
					out = xrealloc(out, out_cap);
				}
				memcpy(out + out_len, utf + seg_start, seg_len);
				out_len += seg_len;
			}
		}
		if (i < len && (utf[i] == '/' || utf[i] == '\\')) {
			if (out_len + 2 > out_cap) {
				out_cap += 64;
				out = xrealloc(out, out_cap);
			}
			out[out_len++] = utf[i++];
		}
	}
	if (!changed) {
		free(out);
		return NULL;
	}
	out[out_len] = '\0';
	return out;
}

static bool has_unencoded_qmark(const char *utf, const char *cjk)
{
	if (!utf || !cjk)
		return false;
	size_t uq = 0, cq = 0;
	for (const char *p = utf; *p; p++) {
		if (*p == '?') uq++;
	}
	for (const char *p = cjk; *p; p++) {
		if (*p == '?') cq++;
	}
	return cq > uq;
}

char *utf2filename_cjk(const char *src, size_t len)
{
	if (!src)
		return NULL;
	if (!len)
		len = strlen(src);
	char *primary = utf2cjk(src, len, sys4_text_is_gbk);
	if (sys4_text_is_gbk && has_unencoded_qmark(src, primary)) {
		char *sjis = utf2cjk(src, len, false);
		if (sjis && !has_unencoded_qmark(src, sjis)) {
			free(primary);
			return sjis;
		}
		free(sjis);
	}
	return primary;
}

/* src 内に半角カナもしくはASCII文字があるかどうか */
bool sjis_has_hankaku(const char *_src) {
	const uint8_t *src = (uint8_t*)_src;
	while(*src) {
		if (SJIS_2BYTE(*src) && src[1]) {
			src++;
		} else {
			return true;
		}
		src++;
	}
	return false;
}

/* src 内に 全角文字があるかどうか */
bool sjis_has_zenkaku(const char *_src) {
	const uint8_t *src = (uint8_t*)_src;
	while(*src) {
		if (SJIS_2BYTE(*src) && src[1]) {
			return true;
		}
		src++;
	}
	return false;
}

/* src 中の文字数を数える 全角文字も１文字 */
int sjis_count_char(const char *_src) {
	const uint8_t *src = (uint8_t*)_src;
	int c = 0;

	while(*src) {
		if (SJIS_2BYTE(*src) && src[1]) {
			src++;
		}
		c++; src++;
	}
	return c;
}

// Replaces lowercase letters with uppercase letters and slashes with backslashes.
void sjis_normalize_path(char *_src) {
	for (uint8_t *src = (uint8_t*)_src; *src; src++) {
		if (SJIS_2BYTE(*src) && src[1]) {
			src++;
		} else if ('a' <= *src && *src <= 'z') {
			*src -= 'a' - 'A';
		} else if (*src == '/') {
			*src = '\\';
		}
	}
}

char *cjk_gbk_zenkaku_to_sjis_buf(const char *src, size_t len, char *buf, size_t cap, bool *changed_out)
{
	if (changed_out)
		*changed_out = false;
	if (!src)
		return NULL;
	char *out = (buf && len < cap) ? buf : xmalloc(len + 1);
	bool changed = false;
	size_t i = 0;
	while (i < len) {
		uint8_t b = (uint8_t)src[i];
		if (b < 0x80) {
			out[i] = (char)b;
			i++;
			continue;
		}
		if (i + 1 < len) {
			uint8_t b2 = (uint8_t)src[i + 1];
			if (b == 0xa3 && b2 >= 0xb0 && b2 <= 0xb9) {
				out[i] = (char)0x82;
				out[i + 1] = (char)(0x4f + (b2 - 0xb0));
				changed = true;
				i += 2;
				continue;
			}
			if (b == 0xa3 && b2 == 0xad) {
				out[i] = (char)0x81;
				out[i + 1] = (char)0x7c;
				changed = true;
				i += 2;
				continue;
			}
			if (b == 0xa3 && b2 == 0xae) {
				out[i] = (char)0x81;
				out[i + 1] = (char)0x44;
				changed = true;
				i += 2;
				continue;
			}
			if (b == 0xa1 && b2 == 0xa1) {
				out[i] = (char)0x81;
				out[i + 1] = (char)0x40;
				changed = true;
				i += 2;
				continue;
			}
			bool sjis_lead = ((b >= 0x81 && b <= 0x9f) || (b >= 0xe0 && b <= 0xfc));
			bool sjis_trail = ((b2 >= 0x40 && b2 <= 0x7e) || (b2 >= 0x80 && b2 <= 0xfc));
			bool gbk_2byte = (sys4_text_is_gbk && b >= 0xa1 && b <= 0xdf && b2 >= 0xa1 && b2 <= 0xfe);
			if ((sjis_lead && sjis_trail) || gbk_2byte) {
				out[i] = (char)b;
				out[i + 1] = (char)b2;
				i += 2;
				continue;
			}
		}
		out[i] = (char)b;
		i++;
	}
	out[len] = '\0';
	if (changed_out)
		*changed_out = changed;
	return out;
}

#ifdef _WIN32
wchar_t *utf8_to_wchar(const char *str)
{
	int nr_wchars = MultiByteToWideChar(CP_UTF8, 0, str, -1, NULL, 0);
	wchar_t *wstr = xmalloc(nr_wchars * sizeof(wchar_t));
	MultiByteToWideChar(CP_UTF8, 0, str, -1, wstr, nr_wchars);
	return wstr;
}

char *wchar_to_utf8(const wchar_t *wstr)
{
	int nr_chars = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, NULL, 0, NULL, NULL);
	char *str = xmalloc(nr_chars);
	WideCharToMultiByte(CP_UTF8, 0, wstr, -1, str, nr_chars, NULL, NULL);
	return str;
}
#endif
