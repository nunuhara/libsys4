/*
 * utfsjis.h -- utf-8/sjis related function
 *
 * Copyright (C) 1997-1998 Masaki Chikama (Wren) <chikama@kasumi.ipl.mech.nagoya-u.ac.jp>
 *               1998-                           <masaki-c@is.aist-nara.ac.jp>
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
/* $Id: eucsjis.h,v 1.2 2000/09/20 10:33:16 chikama Exp $ */

#ifndef SYSTEM4_UTFSJIS
#define SYSTEM4_UTFSJIS

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

extern bool sys4_text_is_gbk;
/* Source compatibility with libsys4 callers selecting CP936. */
#define use_gbk sys4_text_is_gbk
void sys4_set_text_gbk(bool enabled);
extern bool sys4_source_is_utf8;
void sys4_set_source_utf8(bool enabled);
bool sys4_bytes_look_utf8(const uint8_t *s, size_t len);
char *sys4_source_strdup(const char *src);
struct string;
struct string *sys4_source_make_string(const char *src, size_t len);
#define SJIS_2BYTE(b) (sys4_text_is_gbk ? ((uint8_t)(b) >= 0x81 && (uint8_t)(b) <= 0xfe) \
        : (((b) & 0xe0) == 0x80 || ((b) & 0xe0) == 0xe0))

extern int   sjis_index(const char *src, int index);
extern char *sjis_char2unicode(const char *src, int *dst);
extern char* sjis2utf(const char *src, size_t len);
extern char* cjk2utf(const char *src, size_t len, bool gbk);
extern char* cjk2utf_strict(const char *src, bool gbk, bool *valid_out);
extern char* cjk2utf_strict_buf(const char *src, size_t len, bool gbk, char *buf, size_t buf_cap, bool *valid_out);
/* Returns a same-length display copy, or NULL if unchanged. */
extern char* cjk_mixed_display_to_gbk(const char *text, bool allow_ambiguous);
extern char* cjk_dialog_text_to_utf8(const char *src, size_t len);
extern struct string *cjk_display_log_line(struct string *line);
extern char* sjis_filename2utf(const char *src, size_t len);
extern char* cjk_repair_sjis_as_gbk_utf8(const char *utf);
extern int   cjk_encode_codepoint(int u, bool gbk);
extern char* utf2cjk(const char *src, size_t len, bool gbk);
extern char* utf2sjis(const char *src, size_t len);
extern char* utf2filename_cjk(const char *src, size_t len);
/* Optional Mai/word.mai, Longinus CHS.TBL, or UIF uif_config.json custom Shift-JIS table */
extern bool  sys4_load_mai_word_table_mem(const void *data, size_t size);
extern bool  sys4_load_mai_word_table(const char *path);
extern bool  sys4_load_chs_tbl_mem(const void *data, size_t size);
extern bool  sys4_load_chs_tbl(const char *path);
extern bool  sys4_load_uif_config_mem(const void *data, size_t size);
extern bool  sys4_load_uif_config(const char *path);
extern void  sys4_reset_mai_word_table(void);
extern bool  sys4_has_mai_word_table(void);
extern char* mai_sjis_char2unicode(const char *src, int *dst);
extern char* mai_sjis2utf(const char *src, size_t len);
extern char* mai_cjk2utf_strict(const char *src, bool *valid_out);
extern char* mai_utf2sjis(const char *src, size_t len);
/* Optional XOR-0x49 UI/EX text dictionary (e.g. NightRemasterDict.dat) */
extern bool  sys4_load_text_dict_mem(const void *data, size_t size);
extern bool  sys4_load_text_dict(const char *path);
extern void  sys4_reset_text_dict(void);
extern const char *sys4_text_dict_lookup(const char *src);
extern char *sys4_text_dict_translate_lines(const char *src);
extern bool  sjis_has_hankaku(const char *src);
extern bool  sjis_has_zenkaku(const char *src);
extern int   sjis_count_char(const char *src);
extern void  sjis_normalize_path(char *src);
extern char *cjk_gbk_zenkaku_to_sjis_buf(const char *src, size_t len, char *buf, size_t cap, bool *changed_out);

#ifdef _WIN32
wchar_t *utf8_to_wchar(const char *str);
char *wchar_to_utf8(const wchar_t *wstr);
#endif

// Returns (big-endian) SJIS codepoint for first character in a string.
static inline uint16_t sjis_code(const char *_str)
{
	uint8_t *str = (uint8_t*)_str;
	if (SJIS_2BYTE(*str) && str[1]) {
		return (*str << 8) | *(str+1);
	}
	return *str;
}

static inline char *sjis_skip_char(const char *str)
{
	return (char*)(str + (SJIS_2BYTE(*str) && str[1] ? 2 : 1));
}

#endif /* SYSTEM4_UTFSJIS */
