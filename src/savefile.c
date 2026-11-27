/* Copyright (C) 2023 kichikuou <KichikuouChrome@gmail.com>
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

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "little_endian.h"
#include "system4.h"
#include "system4/buffer.h"
#include "system4/file.h"
#include "system4/mt19937int.h"
#include "system4/savefile.h"
#include "system4/string.h"
#include "system4/zlib.h"

#define GD11_ENCRYPT_KEY 0x12320f

static struct rsave_heap_null rsave_null_singleton = { RSAVE_NULL };
struct rsave_heap_null * const rsave_null = &rsave_null_singleton;

static const char *errtab[SAVEFILE_MAX_ERROR] = {
	[SAVEFILE_SUCCESS]            = "Success",
	[SAVEFILE_INVALID_SIGNATURE]  = "Not a System4 save file",
	[SAVEFILE_UNSUPPORTED_FORMAT] = "Unsupported save format",
	[SAVEFILE_INVALID]            = "Invalid save file",
	[SAVEFILE_INTERNAL_ERROR]     = "Internal error",
};

const char *savefile_strerror(enum savefile_error error)
{
	if (error == SAVEFILE_FILE_ERROR)
		return strerror(errno);
	if (error < SAVEFILE_MAX_ERROR)
		return errtab[error];
	return "Invalid error code";
}

void savefile_free(struct savefile *save)
{
	if (!save)
		return;
	free(save->buf);
	free(save);
}

struct savefile *savefile_read(const char *path, enum savefile_error *error)
{
	uint8_t *buf = NULL;
	FILE *fp = file_open_utf8(path, "rb");
	if (!fp) {
		*error = SAVEFILE_FILE_ERROR;
		return NULL;
	}

	struct savefile *save = xcalloc(1, sizeof(struct savefile));
	uint8_t header[8];
	if (fread(header, sizeof(header), 1, fp) != 1) {
		*error = feof(fp) ? SAVEFILE_INVALID_SIGNATURE : SAVEFILE_FILE_ERROR;
		goto err;
	}
	if (memcmp(header, "GD\x01\x01", 4)) {
		*error = SAVEFILE_INVALID_SIGNATURE;
		goto err;
	}
	if (fseek(fp, 0, SEEK_END) != 0) {
		*error = SAVEFILE_FILE_ERROR;
		goto err;
	}
	long end_pos = ftell(fp);
	if (end_pos < (long)sizeof(header) + 2) {
		*error = SAVEFILE_INVALID;
		goto err;
	}
	size_t compressed_size = (size_t)(end_pos - (long)sizeof(header));
	if (fseek(fp, sizeof(header), SEEK_SET) != 0) {
		*error = SAVEFILE_FILE_ERROR;
		goto err;
	}

	buf = xmalloc(compressed_size);
	if (fread(buf, compressed_size, 1, fp) != 1) {
		*error = SAVEFILE_FILE_ERROR;
		goto err;
	}
	fclose(fp);
	fp = NULL;

	if (buf[0] == 0x1a) {
		save->encrypted = true;
		mt19937_xorcode(buf, compressed_size, GD11_ENCRYPT_KEY);
	}
	switch (buf[1]) {
	case 0x01: save->compression_level = ZLIB_BEST_SPEED; break;
	case 0xda: save->compression_level = ZLIB_BEST_COMPRESSION; break;
	default:   save->compression_level = ZLIB_DEFAULT_COMPRESSION; break;
	}

	uint32_t raw_size = (uint32_t)LittleEndian_getDW(header, 4);
	if (raw_size > 256u * 1024u * 1024u) {
		*error = SAVEFILE_INVALID;
		goto err;
	}
	save->buf = xmalloc((size_t)raw_size + 1);
	if (!zlib_decompress_exact(save->buf, raw_size, buf, compressed_size)) {
		*error = SAVEFILE_INVALID;
		goto err;
	}
	save->buf[raw_size] = '\0';
	free(buf);

	save->len = raw_size;
	*error = SAVEFILE_SUCCESS;
	return save;

 err:
	free(save->buf);
	free(save);
	free(buf);
	if (fp)
		fclose(fp);
	return NULL;
}

enum savefile_error savefile_write(struct savefile *save, FILE *out)
{
	size_t bufsize = zlib_compress_bound(save->len);
	uint8_t *buf = xmalloc(bufsize);
	bufsize = zlib_compress(buf, bufsize, save->buf, save->len, save->compression_level);
	if (!bufsize) {
		free(buf);
		return SAVEFILE_INTERNAL_ERROR;
	}

	if (save->encrypted)
		mt19937_xorcode(buf, bufsize, GD11_ENCRYPT_KEY);

	uint8_t header[8] = "GD\x01\x01";
	LittleEndian_putDW(header, 4, save->len);
	bool ok = fwrite(header, sizeof(header), 1, out) == 1 &&
		fwrite(buf, bufsize, 1, out) == 1;
	free(buf);
	return ok ? SAVEFILE_SUCCESS : SAVEFILE_FILE_ERROR;
}

struct gsave *gsave_create(int version, const char *key, int nr_ain_globals, const char *group)
{
	struct gsave *gs = xcalloc(1, sizeof(struct gsave));
	gs->key = strdup(key);
	gs->uk1 = 1000;
	gs->version = version;
	gs->uk2 = version >= 8 ? 64 : 56;
	gs->nr_ain_globals = nr_ain_globals;
	if (version >= 5)
		gs->group = strdup(group ? group : "");
	return gs;
}

void gsave_free(struct gsave *gs)
{
	if (!gs)
		return;
	free(gs->key);
	free(gs->group);
	if (gs->records) {
		for (int32_t i = 0; i < gs->nr_records; i++) {
			free(gs->records[i].struct_name);
			free(gs->records[i].indices);
		}
		free(gs->records);
	}
	if (gs->globals) {
		for (int32_t i = 0; i < gs->nr_globals; i++) {
			free(gs->globals[i].name);
		}
		free(gs->globals);
	}
	if (gs->strings) {
		for (int32_t i = 0; i < gs->nr_strings; i++) {
			if (gs->strings[i])
				free_string(gs->strings[i]);
		}
		free(gs->strings);
	}
	if (gs->arrays) {
		for (int32_t i = 0; i < gs->nr_arrays; i++) {
			free(gs->arrays[i].dimensions);
			if (gs->arrays[i].flat_arrays) {
				for (int32_t j = 0; j < gs->arrays[i].nr_flat_arrays; j++) {
					free(gs->arrays[i].flat_arrays[j].values);
				}
				free(gs->arrays[i].flat_arrays);
			}
		}
		free(gs->arrays);
	}
	if (gs->keyvals) {
		for (int32_t i = 0; i < gs->nr_keyvals; i++) {
			free(gs->keyvals[i].name);
		}
		free(gs->keyvals);
	}
	if (gs->struct_defs) {
		for (int32_t i = 0; i < gs->nr_struct_defs; i++) {
			struct gsave_struct_def *sd = &gs->struct_defs[i];
			free(sd->name);
			if (sd->fields) {
				for (int32_t j = 0; j < sd->nr_fields; j++) {
					free(sd->fields[j].name);
				}
				free(sd->fields);
			}
		}
		free(gs->struct_defs);
	}
	if (gs->comments) {
		for (int32_t i = 0; i < gs->nr_comments; i++) {
			if (gs->comments[i])
				free_string(gs->comments[i]);
		}
		free(gs->comments);
	}
	free(gs);
}

struct gsave *gsave_read(const char *path, enum savefile_error *error)
{
	struct savefile *save = savefile_read(path, error);
	if (!save)
		return NULL;

	struct gsave *gs = xcalloc(1, sizeof(struct gsave));
	*error = gsave_parse(save->buf, save->len, gs);
	if (*error != SAVEFILE_SUCCESS) {
		gsave_free(gs);
		gs = NULL;
	}
	savefile_free(save);
	return gs;
}

static bool sf_read_i32(struct buffer *r, int32_t *out)
{
	if (buffer_remaining(r) < 4)
		return false;
	*out = LittleEndian_getDW(r->buf, r->index);
	r->index += 4;
	return true;
}

static bool sf_read_cstr(struct buffer *r, char **out)
{
	size_t rem = buffer_remaining(r);
	if (rem == 0)
		return false;
	const uint8_t *start = r->buf + r->index;
	const uint8_t *nul = memchr(start, '\0', rem);
	if (!nul)
		return false;
	size_t slen = (size_t)(nul - start);
	char *s = xmalloc(slen + 1);
	memcpy(s, start, slen + 1);
	r->index += slen + 1;
	*out = s;
	return true;
}

static bool sf_read_string(struct buffer *r, struct string **out)
{
	size_t rem = buffer_remaining(r);
	if (rem == 0)
		return false;
	const uint8_t *start = r->buf + r->index;
	const uint8_t *nul = memchr(start, '\0', rem);
	if (!nul)
		return false;
	size_t slen = (size_t)(nul - start);
	if (slen > INT32_MAX)
		return false;
	*out = make_string((const char *)start, slen);
	r->index += slen + 1;
	return true;
}

static bool sf_read_bytes(struct buffer *r, void *dst, size_t n)
{
	if (buffer_remaining(r) < n)
		return false;
	if (n > 0)
		memcpy(dst, r->buf + r->index, n);
	r->index += n;
	return true;
}

static bool gsave_validate_value(int32_t val, enum ain_data_type type, struct gsave *gs)
{
	switch (type) {
	case AIN_VOID:
	case AIN_INT:
	case AIN_BOOL:
	case AIN_FUNC_TYPE:
	case AIN_DELEGATE:
	case AIN_LONG_INT:
	case AIN_FLOAT:
	case AIN_ENUM:
	case AIN_ENUM2:
	case AIN_REF_TYPE:
		return true;
	case AIN_STRING:
		return (0 <= val && val < gs->nr_strings) || val == GSAVE7_EMPTY_STRING;
	case AIN_STRUCT:
		return 0 <= val && val < gs->nr_records;
	case AIN_ARRAY_TYPE:
	case AIN_ARRAY:
		return 0 <= val && val < gs->nr_arrays;
	default:
		return false;
	}
}

enum savefile_error gsave_parse(uint8_t *buf, size_t len, struct gsave *gs)
{
	struct buffer r;
	buffer_init(&r, buf, len);

	if (!sf_read_cstr(&r, &gs->key) ||
	    !sf_read_i32(&r, &gs->uk1) ||
	    !sf_read_i32(&r, &gs->version))
		return SAVEFILE_INVALID;
	if (gs->version != 4 && gs->version != 5 && gs->version != 7 && gs->version != 8)
		return SAVEFILE_UNSUPPORTED_FORMAT;
	if (!sf_read_i32(&r, &gs->uk2) ||
	    !sf_read_i32(&r, &gs->nr_ain_globals) ||
	    gs->nr_ain_globals < 0)
		return SAVEFILE_INVALID;

	int32_t records_off_i32, globals_off_i32, strings_off_i32, arrays_off_i32, keyvals_off_i32;
	int32_t nr_records, nr_globals, nr_strings, nr_arrays, nr_keyvals;
	if (!sf_read_i32(&r, &records_off_i32) || !sf_read_i32(&r, &nr_records) ||
	    !sf_read_i32(&r, &globals_off_i32) || !sf_read_i32(&r, &nr_globals) ||
	    !sf_read_i32(&r, &strings_off_i32) || !sf_read_i32(&r, &nr_strings) ||
	    !sf_read_i32(&r, &arrays_off_i32)  || !sf_read_i32(&r, &nr_arrays) ||
	    !sf_read_i32(&r, &keyvals_off_i32) || !sf_read_i32(&r, &nr_keyvals))
		return SAVEFILE_INVALID;

	if (records_off_i32 < 0 || (size_t)records_off_i32 > len ||
	    globals_off_i32 < 0 || (size_t)globals_off_i32 > len ||
	    strings_off_i32 < 0 || (size_t)strings_off_i32 > len ||
	    arrays_off_i32 < 0  || (size_t)arrays_off_i32 > len ||
	    keyvals_off_i32 < 0 || (size_t)keyvals_off_i32 > len)
		return SAVEFILE_INVALID;

	if (nr_records < 0 || (size_t)nr_records > len / 8 ||
	    nr_globals < 0 || (size_t)nr_globals > len / 9 ||
	    nr_strings < 0 || (size_t)nr_strings > len ||
	    nr_arrays < 0  || (size_t)nr_arrays > len / 8 ||
	    nr_keyvals < 0 || (size_t)nr_keyvals > len / 4)
		return SAVEFILE_INVALID;

	size_t records_offset = (size_t)records_off_i32;
	size_t globals_offset = (size_t)globals_off_i32;
	size_t strings_offset = (size_t)strings_off_i32;
	size_t arrays_offset = (size_t)arrays_off_i32;
	size_t keyvals_offset = (size_t)keyvals_off_i32;
	gs->nr_records = nr_records;
	gs->nr_globals = nr_globals;
	gs->nr_strings = nr_strings;
	gs->nr_arrays = nr_arrays;
	gs->nr_keyvals = nr_keyvals;

	size_t comments_offset = 0;
	if (gs->version >= 8) {
		int32_t comments_off_i32, nr_comments;
		if (!sf_read_i32(&r, &comments_off_i32) || !sf_read_i32(&r, &nr_comments))
			return SAVEFILE_INVALID;
		if (comments_off_i32 < 0 || (size_t)comments_off_i32 > len ||
		    nr_comments < 0 || (size_t)nr_comments > len / 4)
			return SAVEFILE_INVALID;
		comments_offset = (size_t)comments_off_i32;
		gs->nr_comments = nr_comments;
	}

	if (gs->version >= 5) {
		if (!sf_read_cstr(&r, &gs->group))
			return SAVEFILE_INVALID;
	}

	// records
	if (r.index != records_offset)
		return SAVEFILE_INVALID;
	gs->records = xcalloc(gs->nr_records, sizeof(struct gsave_record));
	for (struct gsave_record *rec = gs->records; rec < gs->records + gs->nr_records; rec++) {
		if (gs->version <= 5) {
			int32_t rec_type;
			if (!sf_read_i32(&r, &rec_type) || !sf_read_cstr(&r, &rec->struct_name))
				return SAVEFILE_INVALID;
			rec->type = rec_type;
		} else {
			if (!sf_read_i32(&r, &rec->struct_index))
				return SAVEFILE_INVALID;
			rec->type = rec->struct_index == -1 ? GSAVE_RECORD_GLOBALS : GSAVE_RECORD_STRUCT;
		}
		int32_t nr_indices;
		if (!sf_read_i32(&r, &nr_indices) || nr_indices < 0 || (size_t)nr_indices > buffer_remaining(&r) / 4)
			return SAVEFILE_INVALID;
		rec->nr_indices = nr_indices;
		rec->indices = xcalloc(rec->nr_indices, sizeof(int32_t));
		int index_ubound;
		switch (rec->type) {
		case GSAVE_RECORD_STRUCT:
			index_ubound = gs->nr_keyvals;
			break;
		case GSAVE_RECORD_GLOBALS:
			index_ubound = gs->nr_globals;
			break;
		default:
			return SAVEFILE_INVALID;
		}
		for (int i = 0; i < rec->nr_indices; i++) {
			int32_t index;
			if (!sf_read_i32(&r, &index) || index < 0 || index >= index_ubound)
				return SAVEFILE_INVALID;
			rec->indices[i] = index;
		}
	}

	// globals
	if (r.index != globals_offset)
		return SAVEFILE_INVALID;
	gs->globals = xcalloc(gs->nr_globals, sizeof(struct gsave_global));
	for (struct gsave_global *g = gs->globals; g < gs->globals + gs->nr_globals; g++) {
		int32_t gtype;
		if (!sf_read_i32(&r, &gtype) ||
		    !sf_read_i32(&r, &g->value) ||
		    !sf_read_cstr(&r, &g->name))
			return SAVEFILE_INVALID;
		g->type = gtype;
		if (gs->version <= 5) {
			if (!sf_read_i32(&r, &g->unknown))
				return SAVEFILE_INVALID;
		}
		if (!gsave_validate_value(g->value, g->type, gs))
			return SAVEFILE_INVALID;
	}

	// strings
	if (r.index != strings_offset)
		return SAVEFILE_INVALID;
	gs->strings = xcalloc(gs->nr_strings, sizeof(struct string *));
	for (int i = 0; i < gs->nr_strings; i++) {
		if (!sf_read_string(&r, &gs->strings[i]))
			return SAVEFILE_INVALID;
	}

	// arrays
	if (r.index != arrays_offset)
		return SAVEFILE_INVALID;
	gs->arrays = xcalloc(gs->nr_arrays, sizeof(struct gsave_array));
	for (struct gsave_array *a = gs->arrays; a < gs->arrays + gs->nr_arrays; a++) {
		int32_t rank;
		if (!sf_read_i32(&r, &rank) || rank < -1)
			return SAVEFILE_INVALID;
		a->rank = rank;
		int32_t expected_nr_flat_arrays = 0;
		if (a->rank > 0) {
			if ((size_t)a->rank > buffer_remaining(&r) / 4)
				return SAVEFILE_INVALID;
			expected_nr_flat_arrays = 1;
			a->dimensions = xcalloc(a->rank, sizeof(int32_t));
			for (int i = 0; i < a->rank; i++) {
				int32_t dim;
				if (!sf_read_i32(&r, &dim) || dim < 0)
					return SAVEFILE_INVALID;
				a->dimensions[i] = dim;
				if (i != 0) {
					if (__builtin_mul_overflow(expected_nr_flat_arrays, dim, &expected_nr_flat_arrays))
						return SAVEFILE_INVALID;
				}
			}
		}
		int32_t nr_flat_arrays;
		if (!sf_read_i32(&r, &nr_flat_arrays) || nr_flat_arrays != expected_nr_flat_arrays)
			return SAVEFILE_INVALID;
		if ((size_t)nr_flat_arrays > buffer_remaining(&r) / 4)
			return SAVEFILE_INVALID;
		a->nr_flat_arrays = nr_flat_arrays;
		a->flat_arrays = xcalloc(a->nr_flat_arrays, sizeof(struct gsave_flat_array));
		for (struct gsave_flat_array *fa = a->flat_arrays; fa < a->flat_arrays + a->nr_flat_arrays; fa++) {
			int32_t nr_values;
			if (!sf_read_i32(&r, &nr_values) || nr_values != a->dimensions[0])
				return SAVEFILE_INVALID;
			if (gs->version >= 7) {
				int32_t ftype;
				if (!sf_read_i32(&r, &ftype))
					return SAVEFILE_INVALID;
				fa->type = ftype;
			}
			size_t val_bytes = gs->version >= 7 ? 4 : 8;
			if ((size_t)nr_values > buffer_remaining(&r) / val_bytes)
				return SAVEFILE_INVALID;
			fa->nr_values = nr_values;
			fa->values = xcalloc(fa->nr_values, sizeof(struct gsave_array_value));
			for (int i = 0; i < fa->nr_values; i++) {
				int32_t value;
				if (!sf_read_i32(&r, &value))
					return SAVEFILE_INVALID;
				enum ain_data_type type;
				if (gs->version >= 7) {
					type = fa->type;
				} else {
					int32_t t;
					if (!sf_read_i32(&r, &t))
						return SAVEFILE_INVALID;
					type = t;
				}
				if (!gsave_validate_value(value, type, gs))
					return SAVEFILE_INVALID;
				fa->values[i].value = value;
				fa->values[i].type = type;
			}
		}
	}

	// key-values
	if (r.index != keyvals_offset)
		return SAVEFILE_INVALID;
	gs->keyvals = xcalloc(gs->nr_keyvals, sizeof(struct gsave_keyval));
	for (struct gsave_keyval *kv = gs->keyvals; kv < gs->keyvals + gs->nr_keyvals; kv++) {
		if (gs->version <= 5) {
			int32_t kvtype;
			if (!sf_read_i32(&r, &kvtype) ||
			    !sf_read_i32(&r, &kv->value) ||
			    !sf_read_cstr(&r, &kv->name))
				return SAVEFILE_INVALID;
			kv->type = kvtype;
			if (!gsave_validate_value(kv->value, kv->type, gs))
				return SAVEFILE_INVALID;
		} else {
			if (!sf_read_i32(&r, &kv->value))
				return SAVEFILE_INVALID;
		}
	}

	// struct-defs
	if (gs->version >= 7) {
		int32_t nr_struct_defs;
		if (!sf_read_i32(&r, &nr_struct_defs) || nr_struct_defs < 0 ||
		    (size_t)nr_struct_defs > buffer_remaining(&r) / 5)
			return SAVEFILE_INVALID;
		gs->nr_struct_defs = nr_struct_defs;
		gs->struct_defs = xcalloc(gs->nr_struct_defs, sizeof(struct gsave_struct_def));
		for (struct gsave_struct_def *sd = gs->struct_defs; sd < gs->struct_defs + gs->nr_struct_defs; sd++) {
			int32_t nr_fields;
			if (!sf_read_cstr(&r, &sd->name) ||
			    !sf_read_i32(&r, &nr_fields) ||
			    nr_fields < 0 || (size_t)nr_fields > buffer_remaining(&r) / 5)
				return SAVEFILE_INVALID;
			sd->nr_fields = nr_fields;
			sd->fields = xcalloc(sd->nr_fields, sizeof(struct gsave_field_def));
			for (struct gsave_field_def *fd = sd->fields; fd < sd->fields + sd->nr_fields; fd++) {
				int32_t ftype;
				if (!sf_read_i32(&r, &ftype) || !sf_read_cstr(&r, &fd->name))
					return SAVEFILE_INVALID;
				fd->type = ftype;
			}
		}
	}

	// comments
	if (gs->version >= 8) {
		if (r.index != comments_offset)
			return SAVEFILE_INVALID;
		gs->comments = xcalloc(gs->nr_comments, sizeof(struct string *));
		for (int i = 0; i < gs->nr_comments; i++) {
			int32_t clen;
			if (!sf_read_i32(&r, &clen) || clen < 0 || buffer_remaining(&r) < (size_t)clen)
				return SAVEFILE_INVALID;
			gs->comments[i] = make_string(buffer_strdata(&r), clen);
			buffer_skip(&r, clen);
		}
	}

	return SAVEFILE_SUCCESS;
}

static size_t skip_int32(struct buffer *out)
{
	size_t loc = out->index;
	buffer_write_int32(out, 0);
	return loc;
}

enum savefile_error gsave_write(struct gsave *gs, FILE *out, bool encrypt, int compression_level)
{
	struct buffer w;
	buffer_init(&w, NULL, 0);
	buffer_write_cstringz(&w, gs->key);
	buffer_write_int32(&w, gs->uk1);
	buffer_write_int32(&w, gs->version);
	buffer_write_int32(&w, gs->uk2);
	buffer_write_int32(&w, gs->nr_ain_globals);

	size_t records_offset_loc = skip_int32(&w);
	buffer_write_int32(&w, gs->nr_records);
	size_t globals_offset_loc = skip_int32(&w);
	buffer_write_int32(&w, gs->nr_globals);
	size_t strings_offset_loc = skip_int32(&w);
	buffer_write_int32(&w, gs->nr_strings);
	size_t arrays_offset_loc = skip_int32(&w);
	buffer_write_int32(&w, gs->nr_arrays);
	size_t keyvals_offset_loc = skip_int32(&w);
	buffer_write_int32(&w, gs->nr_keyvals);
	size_t comments_offset_loc = 0;
	if (gs->version >= 8) {
		comments_offset_loc = skip_int32(&w);
		buffer_write_int32(&w, gs->nr_comments);
	}

	if (gs->version >= 5)
		buffer_write_cstringz(&w, gs->group);

	// records
	buffer_write_int32_at(&w, records_offset_loc, w.index);
	for (struct gsave_record *r = gs->records; r < gs->records + gs->nr_records; r++) {
		if (gs->version <= 5) {
			buffer_write_int32(&w, r->type);
			buffer_write_cstringz(&w, r->struct_name);
		} else {
			buffer_write_int32(&w, r->struct_index);
		}
		buffer_write_int32(&w, r->nr_indices);
		for (int i = 0; i < r->nr_indices; i++)
			buffer_write_int32(&w, r->indices[i]);
	}

	// globals
	buffer_write_int32_at(&w, globals_offset_loc, w.index);
	for (struct gsave_global *g = gs->globals; g < gs->globals + gs->nr_globals; g++) {
		buffer_write_int32(&w, g->type);
		buffer_write_int32(&w, g->value);
		buffer_write_cstringz(&w, g->name);
		if (gs->version <= 5)
			buffer_write_int32(&w, g->unknown);
	}

	// strings
	buffer_write_int32_at(&w, strings_offset_loc, w.index);
	for (int i = 0; i < gs->nr_strings; i++) {
		buffer_write_cstringz(&w, gs->strings[i]->text);
	}

	// arrays
	buffer_write_int32_at(&w, arrays_offset_loc, w.index);
	for (struct gsave_array *a = gs->arrays; a < gs->arrays + gs->nr_arrays; a++) {
		buffer_write_int32(&w, a->rank);
		for (int i = 0; i < a->rank; i++)
			buffer_write_int32(&w, a->dimensions[i]);
		buffer_write_int32(&w, a->nr_flat_arrays);
		for (struct gsave_flat_array *fa = a->flat_arrays; fa < a->flat_arrays + a->nr_flat_arrays; fa++) {
			buffer_write_int32(&w, fa->nr_values);
			if (gs->version >= 7)
				buffer_write_int32(&w, fa->type);
			for (int i = 0; i < fa->nr_values; i++) {
				buffer_write_int32(&w, fa->values[i].value);
				if (gs->version <= 5)
					buffer_write_int32(&w, fa->values[i].type);
			}
		}
	}

	// key-values
	buffer_write_int32_at(&w, keyvals_offset_loc, w.index);
	for (struct gsave_keyval *kv = gs->keyvals; kv < gs->keyvals + gs->nr_keyvals; kv++) {
		if (gs->version <= 5)
			buffer_write_int32(&w, kv->type);
		buffer_write_int32(&w, kv->value);
		if (gs->version <= 5)
			buffer_write_cstringz(&w, kv->name);
	}

	// struct-defs
	if (gs->version >= 7) {
		buffer_write_int32(&w, gs->nr_struct_defs);
		for (struct gsave_struct_def *sd = gs->struct_defs; sd < gs->struct_defs + gs->nr_struct_defs; sd++) {
			buffer_write_cstringz(&w, sd->name);
			buffer_write_int32(&w, sd->nr_fields);
			for (struct gsave_field_def *fd = sd->fields; fd < sd->fields + sd->nr_fields; fd++) {
				buffer_write_int32(&w, fd->type);
				buffer_write_cstringz(&w, fd->name);
			}
		}
	}

	// comments
	if (gs->version >= 8) {
		buffer_write_int32_at(&w, comments_offset_loc, w.index);
		for (int i = 0; i < gs->nr_comments; i++) {
			buffer_write_pascal_string(&w, gs->comments[i]);
		}
	}
	struct savefile save = {
		.buf = w.buf,
		.len = w.index,
		.encrypted = encrypt,
		.compression_level = compression_level
	};
	enum savefile_error err = savefile_write(&save, out);
	free(w.buf);
	return err;
}

int32_t gsave_add_globals_record(struct gsave *gs, int nr_globals)
{
	assert(gs->nr_globals == 0);
	struct gsave_record rec = {
		.struct_index = -1,
		.type = GSAVE_RECORD_GLOBALS,
		.struct_name = strdup(""),
		.nr_indices = nr_globals,
		.indices = xcalloc(nr_globals, sizeof(int32_t)),
	};
	for (int i = 0; i < nr_globals; i++)
		rec.indices[i] = i;
	gsave_add_record(gs, &rec);
	gs->nr_globals = nr_globals + (gs->version >= 8);
	gs->globals = xcalloc(gs->nr_globals, sizeof(struct gsave_global));
	if (gs->version >= 8) gs->globals[nr_globals].name = strdup("");
	for (int i = 0; i < nr_globals; i++)
		gs->globals[i].unknown = 1;
	return 0;
}

int32_t gsave_add_record(struct gsave *gs, struct gsave_record *rec)
{
	int32_t n = gs->nr_records++;
	if (gs->nr_records > gs->cap_records) {
		gs->cap_records = max(gs->nr_records, gs->cap_records * 2);
		gs->records = xrealloc_array(gs->records, n, gs->cap_records, sizeof(struct gsave_record));
	}
	gs->records[n] = *rec;
	return n;
}

int32_t gsave_add_string(struct gsave *gs, struct string *s)
{
	if (gs->version >= 7 && s->size == 0)
		return GSAVE7_EMPTY_STRING;
	int n = gs->nr_strings++;
	if (gs->nr_strings > gs->cap_strings) {
		gs->cap_strings = max(gs->nr_strings, gs->cap_strings * 2);
		gs->strings = xrealloc_array(gs->strings, n, gs->cap_strings, sizeof(struct string*));
	}
	gs->strings[n] = string_ref(s);
	return n;
}

int32_t gsave_add_array(struct gsave *gs, struct gsave_array *array)
{
	int n = gs->nr_arrays++;
	if (gs->nr_arrays > gs->cap_arrays) {
		gs->cap_arrays = max(gs->nr_arrays, gs->cap_arrays * 2);
		gs->arrays = xrealloc_array(gs->arrays, n, gs->cap_arrays, sizeof(struct gsave_array));
	}
	gs->arrays[n] = *array;
	return n;
}

int32_t gsave_add_keyval(struct gsave *gs, struct gsave_keyval *kv)
{
	int n = gs->nr_keyvals++;
	if (gs->nr_keyvals > gs->cap_keyvals) {
		gs->cap_keyvals = max(gs->nr_keyvals, gs->cap_keyvals * 2);
		gs->keyvals = xrealloc_array(gs->keyvals, n, gs->cap_keyvals, sizeof(struct gsave_keyval));
	}
	gs->keyvals[n] = *kv;
	return n;
}

int32_t gsave_add_struct_def(struct gsave *gs, struct ain_struct *st)
{
	int n = gs->nr_struct_defs++;
	if (gs->nr_struct_defs > gs->cap_struct_defs) {
		gs->cap_struct_defs = max(gs->nr_struct_defs, gs->cap_struct_defs * 2);
		gs->struct_defs = xrealloc_array(gs->struct_defs, n, gs->cap_struct_defs, sizeof(struct gsave_struct_def));
	}

	struct gsave_struct_def *sd = &gs->struct_defs[n];
	sd->name = strdup(st->name);
	sd->nr_fields = st->nr_members;
	sd->fields = xcalloc(st->nr_members, sizeof(struct gsave_field_def));

	for (int i = 0; i < st->nr_members; i++) {
		const char *field_name = st->members[i].name2 ? st->members[i].name2 : st->members[i].name;
		sd->fields[i].type = st->members[i].type.data;
		sd->fields[i].name = strdup(field_name);
	}

	return n;
}

int32_t gsave_get_struct_def(struct gsave *gs, const char *name)
{
	for (int i = 0; i < gs->nr_struct_defs; i++) {
		if (!strcmp(gs->struct_defs[i].name, name))
			return i;
	}
	return -1;
}

static void rsave_free_frame(struct rsave_heap_frame *f)
{
	free(f->func.name);
	free(f->types);
	free(f);
}

static void rsave_free_string(struct rsave_heap_string *s)
{
	free(s);
}

static void rsave_free_array(struct rsave_heap_array *a)
{
	free(a->struct_type.name);
	free(a);
}

static void rsave_free_struct(struct rsave_heap_struct *s)
{
	free(s->ctor.name);
	free(s->dtor.name);
	free(s->struct_type.name);
	free(s->types);
	free(s->ctor_flags);
	free(s);
}

static void rsave_free_delegate(struct rsave_heap_delegate *d)
{
	for (int i = 0; i < d->nr_entries; i++)
		free(d->entries[i].function.name);
	free(d->entries);
	free(d);
}

void rsave_free(struct rsave *rs)
{
	if (!rs)
		return;
	free(rs->key);
	if (rs->comments) {
		for (int i = 0; i < rs->nr_comments; i++)
			free(rs->comments[i]);
		free(rs->comments);
	}
	free(rs->ip.caller_func);
	free(rs->stack);
	free(rs->call_frames);
	if (rs->return_records) {
		for (int i = 0; i < rs->nr_return_records; i++)
			free(rs->return_records[i].caller_func);
		free(rs->return_records);
	}
	if (rs->heap) {
		for (int i = 0; i < rs->nr_heap_objs; i++) {
			enum rsave_heap_tag *tag = rs->heap[i];
			if (!tag) continue;
			switch (*tag) {
			case RSAVE_GLOBALS:
			case RSAVE_LOCALS:
				rsave_free_frame(rs->heap[i]);
				break;
			case RSAVE_STRING:
				rsave_free_string(rs->heap[i]);
				break;
			case RSAVE_ARRAY:
				rsave_free_array(rs->heap[i]);
				break;
			case RSAVE_STRUCT:
				rsave_free_struct(rs->heap[i]);
				break;
			case RSAVE_DELEGATE:
				rsave_free_delegate(rs->heap[i]);
				break;
			case RSAVE_NULL:
				assert(rs->heap[i] == rsave_null);
				break;
			default:
				ERROR("unknown rsave heap tag %d", *tag);
			}
		}
		free(rs->heap);
	}
	if (rs->func_names) {
		for (int i = 0; i < rs->nr_func_names; i++)
			free(rs->func_names[i]);
		free(rs->func_names);
	}
	free(rs);
}

struct rsave *rsave_read(const char *path, enum rsave_read_mode mode, enum savefile_error *error)
{
	struct savefile *save = savefile_read(path, error);
	if (!save)
		return NULL;

	struct rsave *rs = xcalloc(1, sizeof(struct rsave));
	*error = rsave_parse(save->buf, save->len, mode, rs);
	if (*error != SAVEFILE_SUCCESS) {
		rsave_free(rs);
		rs = NULL;
	}
	savefile_free(save);
	return rs;
}

static bool parse_int_array(struct buffer *r, int32_t *num, int32_t **out)
{
	int32_t n;
	if (!sf_read_i32(r, &n) || n < 0 || (size_t)n > buffer_remaining(r) / 4)
		return false;
	int32_t *buf = xcalloc(n, sizeof(int32_t));
	for (int32_t i = 0; i < n; i++) {
		if (!sf_read_i32(r, &buf[i])) {
			free(buf);
			return false;
		}
	}
	*num = n;
	*out = buf;
	return true;
}

static bool parse_string_array(struct buffer *r, int32_t *num, char ***out)
{
	int32_t n;
	if (!sf_read_i32(r, &n) || n < 0 || (size_t)n > buffer_remaining(r))
		return false;
	char **strs = xcalloc(n, sizeof(char*));
	for (int32_t i = 0; i < n; i++) {
		if (!sf_read_cstr(r, &strs[i])) {
			for (int32_t j = 0; j < i; j++)
				free(strs[j]);
			free(strs);
			return false;
		}
	}
	*num = n;
	*out = strs;
	return true;
}

static bool parse_rsave_symbol(struct buffer *r, int32_t version, struct rsave_symbol *out)
{
	*out = (struct rsave_symbol){0};
	if (version == 4)
		return sf_read_i32(r, &out->id);
	return sf_read_cstr(r, &out->name);
}

static bool parse_call_frames(struct buffer *r, int32_t *num, struct rsave_call_frame **out)
{
	int32_t nr_local_ptrs = 0, nr_frame_types = 0, nr_struct_ptrs = 0;
	int32_t *local_ptrs = NULL, *frame_types = NULL, *struct_ptrs = NULL;
	struct rsave_call_frame *frames = NULL;

	if (!parse_int_array(r, &nr_local_ptrs, &local_ptrs) ||
	    !parse_int_array(r, &nr_frame_types, &frame_types) ||
	    !parse_int_array(r, &nr_struct_ptrs, &struct_ptrs) ||
	    nr_local_ptrs != nr_frame_types)
		goto err;

	frames = xcalloc(nr_local_ptrs, sizeof(struct rsave_call_frame));
	int32_t struct_ptr_index = 0;
	for (int32_t i = 0; i < nr_local_ptrs; i++) {
		frames[i].type = frame_types[i];
		frames[i].local_ptr = local_ptrs[i];
		if (frame_types[i] == RSAVE_METHOD_CALL) {
			if (struct_ptr_index >= nr_struct_ptrs)
				goto err;
			frames[i].struct_ptr = struct_ptrs[struct_ptr_index++];
		} else {
			frames[i].struct_ptr = -1;
		}
	}
	if (struct_ptr_index != nr_struct_ptrs)
		goto err;
	free(local_ptrs);
	free(frame_types);
	free(struct_ptrs);
	*num = nr_local_ptrs;
	*out = frames;
	return true;

err:
	free(local_ptrs);
	free(frame_types);
	free(struct_ptrs);
	free(frames);
	return false;
}

static bool parse_return_record(struct buffer *r, struct rsave_return_record *f)
{
	if (!sf_read_i32(r, &f->return_addr))
		return false;
	if (f->return_addr == -1)
		return true;
	return sf_read_cstr(r, &f->caller_func) &&
	       sf_read_i32(r, &f->local_addr) &&
	       sf_read_i32(r, &f->crc);
}

static struct rsave_heap_frame *parse_heap_frame(struct buffer *r, int32_t version, enum rsave_heap_tag tag)
{
	struct rsave_heap_frame f = { .tag = tag, .struct_ptr = -1, .parent_ptr = -1 };
	if (!sf_read_i32(r, &f.ref))
		goto err;
	if (version >= 9 && version < 14) {
		if (!sf_read_i32(r, &f.seq))
			goto err;
	}
	if (version == 4) {
		if (!sf_read_i32(r, &f.func.id))
			goto err;
	} else if (tag == RSAVE_GLOBALS) {
		if (version == 14) {
			f.func.id = -1;
		} else if (!sf_read_i32(r, &f.func.id)) {
			goto err;
		}
		if (f.func.id != -1)
			goto err;
	} else {
		if (!sf_read_cstr(r, &f.func.name))
			goto err;
	}

	if (version < 14) {
		if (!parse_int_array(r, &f.nr_types, &f.types))
			goto err;
	}
	if (tag == RSAVE_LOCALS && version >= 9) {
		if (!sf_read_i32(r, &f.struct_ptr))
			goto err;
	}
	if (tag == RSAVE_LOCALS && version == 14) {
		if (!sf_read_i32(r, &f.parent_ptr))
			goto err;
	}
	int32_t slots_size;
	if (!sf_read_i32(r, &slots_size) ||
	    slots_size < 0 || (size_t)slots_size > buffer_remaining(r) || slots_size % sizeof(int32_t) != 0)
		goto err;
	f.nr_slots = slots_size / sizeof(int32_t);

	struct rsave_heap_frame *obj = xmalloc(sizeof(struct rsave_heap_frame) + (size_t)slots_size);
	*obj = f;
	for (int i = 0; i < f.nr_slots; i++) {
		if (!sf_read_i32(r, &obj->slots[i])) {
			rsave_free_frame(obj);
			return NULL;
		}
	}
	return obj;

err:
	free(f.func.name);
	free(f.types);
	return NULL;
}

static struct rsave_heap_string *parse_heap_string(struct buffer *r, int32_t version)
{
	struct rsave_heap_string s = { .tag = RSAVE_STRING };
	if (!sf_read_i32(r, &s.ref))
		return NULL;
	if (version >= 9 && version < 14) {
		if (!sf_read_i32(r, &s.seq))
			return NULL;
	}
	if (version < 14) {
		if (!sf_read_i32(r, &s.uk))
			return NULL;
	}
	if (s.uk != 0 && s.uk != 1)
		return NULL;
	if (!sf_read_i32(r, &s.len) || s.len < 1 || (size_t)s.len > buffer_remaining(r))
		return NULL;
	struct rsave_heap_string *obj = xmalloc(sizeof(struct rsave_heap_string) + (size_t)s.len);
	*obj = s;
	if (!sf_read_bytes(r, obj->text, (size_t)s.len)) {
		free(obj);
		return NULL;
	}
	if (obj->text[s.len - 1] != '\0') {
		obj->text[s.len - 1] = '\0';
	}
	return obj;
}

static struct rsave_heap_array *parse_heap_array(struct buffer *r, int32_t version)
{
	struct rsave_heap_array a = { .tag = RSAVE_ARRAY };
	int32_t data_type;
	if (!sf_read_i32(r, &a.ref))
		goto err;
	if (version >= 9 && version < 14) {
		if (!sf_read_i32(r, &a.seq))
			goto err;
	}
	if (!sf_read_i32(r, &a.rank_minus_1) ||
	    !sf_read_i32(r, &data_type) ||
	    !parse_rsave_symbol(r, version, &a.struct_type))
		goto err;
	a.data_type = data_type;
	if (version == 14) {
		if (a.rank_minus_1 == INT32_MAX)
			goto err;
		a.root_rank = a.rank_minus_1 + 1;
	} else if (!sf_read_i32(r, &a.root_rank)) {
		goto err;
	}
	if (!sf_read_i32(r, &a.is_not_empty))
		goto err;

	int32_t slots_size;
	if (!sf_read_i32(r, &slots_size) ||
	    slots_size < 0 || (size_t)slots_size > buffer_remaining(r) || slots_size % sizeof(int32_t) != 0)
		goto err;
	a.nr_slots = slots_size / sizeof(int32_t);
	struct rsave_heap_array *obj = xmalloc(sizeof(struct rsave_heap_array) + (size_t)slots_size);
	*obj = a;
	for (int i = 0; i < a.nr_slots; i++) {
		if (!sf_read_i32(r, &obj->slots[i])) {
			rsave_free_array(obj);
			return NULL;
		}
	}
	return obj;

err:
	free(a.struct_type.name);
	return NULL;
}

static struct rsave_heap_struct *parse_heap_struct(struct buffer *r, int32_t version)
{
	struct rsave_heap_struct s = { .tag = RSAVE_STRUCT };
	if (!sf_read_i32(r, &s.ref))
		goto err;
	if (version >= 9 && version < 14) {
		if (!sf_read_i32(r, &s.seq))
			goto err;
	}
	if (version == 14) {
		if (!parse_int_array(r, &s.nr_ctor_flags, &s.ctor_flags))
			goto err;
	} else {
		if (!parse_rsave_symbol(r, version, &s.ctor) ||
		    !parse_rsave_symbol(r, version, &s.dtor) ||
		    !sf_read_i32(r, &s.uk))
			goto err;
	}
	if (s.uk != 0)
		goto err;
	if (!parse_rsave_symbol(r, version, &s.struct_type))
		goto err;
	if (version < 14) {
		if (!parse_int_array(r, &s.nr_types, &s.types))
			goto err;
	}
	int32_t slots_size;
	if (!sf_read_i32(r, &slots_size) ||
	    slots_size < 0 || (size_t)slots_size > buffer_remaining(r) || slots_size % sizeof(int32_t) != 0)
		goto err;
	s.nr_slots = slots_size / sizeof(int32_t);
	struct rsave_heap_struct *obj = xmalloc(sizeof(struct rsave_heap_struct) + (size_t)slots_size);
	*obj = s;
	for (int i = 0; i < s.nr_slots; i++) {
		if (!sf_read_i32(r, &obj->slots[i])) {
			rsave_free_struct(obj);
			return NULL;
		}
	}
	return obj;

err:
	free(s.ctor.name);
	free(s.dtor.name);
	free(s.struct_type.name);
	free(s.types);
	free(s.ctor_flags);
	return NULL;
}

static struct rsave_heap_delegate *parse_heap_delegate(struct buffer *r, int32_t version)
{
	if (version < 9)
		return NULL;
	struct rsave_heap_delegate d = { .tag = RSAVE_DELEGATE };
	if (!sf_read_i32(r, &d.ref))
		return NULL;
	if (version == 14) {
		if (!sf_read_i32(r, &d.cursor) ||
		    !sf_read_i32(r, &d.end) ||
		    !sf_read_i32(r, &d.nr_entries) ||
		    d.nr_entries < 0 || (size_t)d.nr_entries > buffer_remaining(r) / 9)
			return NULL;
		d.entries = xcalloc(d.nr_entries, sizeof(*d.entries));
		for (int i = 0; i < d.nr_entries; i++) {
			if (!sf_read_i32(r, &d.entries[i].object) ||
			    !parse_rsave_symbol(r, version, &d.entries[i].function) ||
			    !sf_read_i32(r, &d.entries[i].environment)) {
				for (int j = 0; j <= i; j++)
					free(d.entries[j].function.name);
				free(d.entries);
				return NULL;
			}
		}
		struct rsave_heap_delegate *obj = xmalloc(sizeof(*obj));
		*obj = d;
		return obj;
	}
	int32_t slots_size;
	if (!sf_read_i32(r, &d.seq) ||
	    !sf_read_i32(r, &slots_size) ||
	    slots_size < 0 || (size_t)slots_size > buffer_remaining(r) || slots_size % sizeof(int32_t) != 0)
		return NULL;
	d.nr_slots = slots_size / sizeof(int32_t);
	struct rsave_heap_delegate *obj = xmalloc(sizeof(struct rsave_heap_delegate) + (size_t)slots_size);
	*obj = d;
	for (int i = 0; i < d.nr_slots; i++) {
		if (!sf_read_i32(r, &obj->slots[i])) {
			free(obj);
			return NULL;
		}
	}
	return obj;
}

enum savefile_error rsave_parse(uint8_t *buf, size_t len, enum rsave_read_mode mode, struct rsave *rs)
{
	struct buffer r;
	buffer_init(&r, buf, len);

	if (len < 8 || memcmp(buffer_strdata(&r), "RSM\0", 4))
		return SAVEFILE_INVALID_SIGNATURE;
	buffer_skip(&r, 4);

	if (!sf_read_i32(&r, &rs->version))
		return SAVEFILE_INVALID;
	if (rs->version != 4 && rs->version != 6 && rs->version != 7 && rs->version != 9 && rs->version != 14)
		return SAVEFILE_UNSUPPORTED_FORMAT;
	if (!sf_read_cstr(&r, &rs->key))
		return SAVEFILE_INVALID;
	if (rs->version >= 7) {
		if (!parse_string_array(&r, &rs->nr_comments, &rs->comments))
			return SAVEFILE_INVALID;
		if (buffer_remaining(&r) == 0) {
			rs->comments_only = true;
			return SAVEFILE_SUCCESS;
		}
	}

	if (mode == RSAVE_READ_COMMENTS) {
		rs->comments_only = true;
		return SAVEFILE_SUCCESS;
	}

	if (!parse_return_record(&r, &rs->ip))
		return SAVEFILE_INVALID;
	if (rs->version < 14) {
		if (!sf_read_i32(&r, &rs->uk1))
			return SAVEFILE_INVALID;
	}
	if (rs->uk1)
		return SAVEFILE_INVALID;
	if (!parse_int_array(&r, &rs->stack_size, &rs->stack) ||
	    !parse_call_frames(&r, &rs->nr_call_frames, &rs->call_frames))
		return SAVEFILE_INVALID;
	int32_t nr_return_records;
	if (!sf_read_i32(&r, &nr_return_records) || nr_return_records < 0 ||
	    (size_t)nr_return_records > buffer_remaining(&r) / 4)
		return SAVEFILE_INVALID;
	rs->nr_return_records = nr_return_records;
	rs->return_records = xcalloc(rs->nr_return_records, sizeof(struct rsave_return_record));
	for (int i = 0; i < rs->nr_return_records; i++) {
		if (!parse_return_record(&r, &rs->return_records[i]))
			return SAVEFILE_INVALID;
	}

	if (!sf_read_i32(&r, &rs->uk2))
		return SAVEFILE_INVALID;
	if (rs->version < 14) {
		if (!sf_read_i32(&r, &rs->uk3) || !sf_read_i32(&r, &rs->uk4))
			return SAVEFILE_INVALID;
	}
	if (rs->version >= 9 && rs->version < 14) {
		if (!sf_read_i32(&r, &rs->next_seq))
			return SAVEFILE_INVALID;
	}
	if (rs->uk2 || rs->uk3 || rs->uk4)
		return SAVEFILE_INVALID;

	int32_t nr_heap_objs;
	if (!sf_read_i32(&r, &nr_heap_objs) || nr_heap_objs < 0 ||
	    (size_t)nr_heap_objs > buffer_remaining(&r) / 4)
		return SAVEFILE_INVALID;
	rs->nr_heap_objs = nr_heap_objs;
	rs->heap = xcalloc(rs->nr_heap_objs, sizeof(void*));
	for (int i = 0; i < rs->nr_heap_objs; i++) {
		int32_t tag_val;
		if (!sf_read_i32(&r, &tag_val))
			return SAVEFILE_INVALID;
		enum rsave_heap_tag tag = tag_val;
		switch (tag) {
		case RSAVE_GLOBALS:
		case RSAVE_LOCALS:
			rs->heap[i] = parse_heap_frame(&r, rs->version, tag);
			break;
		case RSAVE_STRING:
			rs->heap[i] = parse_heap_string(&r, rs->version);
			break;
		case RSAVE_ARRAY:
			rs->heap[i] = parse_heap_array(&r, rs->version);
			break;
		case RSAVE_STRUCT:
			rs->heap[i] = parse_heap_struct(&r, rs->version);
			break;
		case RSAVE_DELEGATE:
			rs->heap[i] = parse_heap_delegate(&r, rs->version);
			break;
		case RSAVE_NULL:
			rs->heap[i] = rsave_null;
			break;
		default:
			return SAVEFILE_INVALID;
		}
		if (!rs->heap[i])
			return SAVEFILE_INVALID;
	}

	if (rs->version >= 6 && rs->version < 14) {
		if (!parse_string_array(&r, &rs->nr_func_names, &rs->func_names))
			return SAVEFILE_INVALID;
	}

	if (buffer_remaining(&r) != 0)
		return SAVEFILE_INVALID;
	return SAVEFILE_SUCCESS;
}

static void write_rsave_symbol(struct buffer *w, struct rsave_symbol *sym)
{
	if (sym->name)
		buffer_write_cstringz(w, sym->name);
	else
		buffer_write_int32(w, sym->id);
}

static void write_return_record(struct buffer *w, struct rsave_return_record *f)
{
	buffer_write_int32(w, f->return_addr);
	if (f->return_addr == -1)
		return;
	buffer_write_cstringz(w, f->caller_func);
	buffer_write_int32(w, f->local_addr);
	buffer_write_int32(w, f->crc);
}

static void write_heap_frame(struct buffer *w, enum rsave_heap_tag tag, int32_t version, struct rsave_heap_frame *f)
{
	buffer_write_int32(w, f->tag);
	buffer_write_int32(w, f->ref);
	if (version >= 9 && version < 14)
		buffer_write_int32(w, f->seq);
	if (version < 14 || tag == RSAVE_LOCALS)
		write_rsave_symbol(w, &f->func);
	if (version < 14) {
		buffer_write_int32(w, f->nr_types);
		for (int i = 0; i < f->nr_types; i++)
			buffer_write_int32(w, f->types[i]);
	}
	if (tag == RSAVE_LOCALS && version >= 9)
		buffer_write_int32(w, f->struct_ptr);
	if (tag == RSAVE_LOCALS && version == 14)
		buffer_write_int32(w, f->parent_ptr);
	buffer_write_int32(w, f->nr_slots * sizeof(int32_t));
	for (int i = 0; i < f->nr_slots; i++)
		buffer_write_int32(w, f->slots[i]);
}

static void write_heap_string(struct buffer *w, int32_t version, struct rsave_heap_string *s)
{
	buffer_write_int32(w, s->tag);
	buffer_write_int32(w, s->ref);
	if (version >= 9 && version < 14)
		buffer_write_int32(w, s->seq);
	if (version < 14)
		buffer_write_int32(w, s->uk);
	buffer_write_int32(w, s->len);
	buffer_write_bytes(w, (uint8_t*)s->text, s->len);
}

static void write_heap_array(struct buffer *w, int32_t version, struct rsave_heap_array *a)
{
	buffer_write_int32(w, a->tag);
	buffer_write_int32(w, a->ref);
	if (version >= 9 && version < 14)
		buffer_write_int32(w, a->seq);
	buffer_write_int32(w, a->rank_minus_1);
	buffer_write_int32(w, a->data_type);
	write_rsave_symbol(w, &a->struct_type);
	if (version < 14)
		buffer_write_int32(w, a->root_rank);
	buffer_write_int32(w, a->is_not_empty);
	buffer_write_int32(w, a->nr_slots * sizeof(int32_t));
	for (int i = 0; i < a->nr_slots; i++)
		buffer_write_int32(w, a->slots[i]);
}

static void write_heap_struct(struct buffer *w, int32_t version, struct rsave_heap_struct *s)
{
	buffer_write_int32(w, s->tag);
	buffer_write_int32(w, s->ref);
	if (version >= 9 && version < 14)
		buffer_write_int32(w, s->seq);
	if (version == 14) {
		buffer_write_int32(w, s->nr_ctor_flags);
		for (int i = 0; i < s->nr_ctor_flags; i++)
			buffer_write_int32(w, s->ctor_flags[i]);
	} else {
		write_rsave_symbol(w, &s->ctor);
		write_rsave_symbol(w, &s->dtor);
		buffer_write_int32(w, s->uk);
	}
	write_rsave_symbol(w, &s->struct_type);
	if (version < 14) {
		buffer_write_int32(w, s->nr_types);
		for (int i = 0; i < s->nr_types; i++)
			buffer_write_int32(w, s->types[i]);
	}
	buffer_write_int32(w, s->nr_slots * sizeof(int32_t));
	for (int i = 0; i < s->nr_slots; i++)
		buffer_write_int32(w, s->slots[i]);
}

static void write_heap_delegate(struct buffer *w, int32_t version, struct rsave_heap_delegate *d)
{
	buffer_write_int32(w, d->tag);
	buffer_write_int32(w, d->ref);
	if (version == 14) {
		buffer_write_int32(w, d->cursor);
		buffer_write_int32(w, d->end);
		buffer_write_int32(w, d->nr_entries);
		for (int i = 0; i < d->nr_entries; i++) {
			buffer_write_int32(w, d->entries[i].object);
			write_rsave_symbol(w, &d->entries[i].function);
			buffer_write_int32(w, d->entries[i].environment);
		}
		return;
	}
	if (version >= 9)
		buffer_write_int32(w, d->seq);
	buffer_write_int32(w, d->nr_slots * sizeof(int32_t));
	for (int i = 0; i < d->nr_slots; i++)
		buffer_write_int32(w, d->slots[i]);
}

enum savefile_error rsave_write(struct rsave *rs, FILE *out, bool encrypt, int compression_level)
{
	struct buffer w;
	buffer_init(&w, NULL, 0);
	buffer_write_cstringz(&w, "RSM");
	buffer_write_int32(&w, rs->version);
	buffer_write_cstringz(&w, rs->key);
	if (rs->version >= 7) {
		buffer_write_int32(&w, rs->nr_comments);
		for (int i = 0; i < rs->nr_comments; i++)
			buffer_write_cstringz(&w, rs->comments[i]);
	}
	if (!rs->comments_only) {
		write_return_record(&w, &rs->ip);
		if (rs->version < 14)
			buffer_write_int32(&w, rs->uk1);
		buffer_write_int32(&w, rs->stack_size);
		for (int i = 0; i < rs->stack_size; i++)
			buffer_write_int32(&w, rs->stack[i]);

		buffer_write_int32(&w, rs->nr_call_frames);
		for (int i = 0; i < rs->nr_call_frames; i++)
			buffer_write_int32(&w, rs->call_frames[i].local_ptr);
		buffer_write_int32(&w, rs->nr_call_frames);
		for (int i = 0; i < rs->nr_call_frames; i++)
			buffer_write_int32(&w, rs->call_frames[i].type);
		size_t nr_struct_ptrs_loc = skip_int32(&w);
		int32_t nr_struct_ptrs = 0;
		for (int i = 0; i < rs->nr_call_frames; i++) {
			if (rs->call_frames[i].type == RSAVE_METHOD_CALL) {
				buffer_write_int32(&w, rs->call_frames[i].struct_ptr);
				nr_struct_ptrs++;
			}
		}
		buffer_write_int32_at(&w, nr_struct_ptrs_loc, nr_struct_ptrs);

		buffer_write_int32(&w, rs->nr_return_records);
		for (int i = 0; i < rs->nr_return_records; i++)
			write_return_record(&w, &rs->return_records[i]);
		buffer_write_int32(&w, rs->uk2);
		if (rs->version < 14) {
			buffer_write_int32(&w, rs->uk3);
			buffer_write_int32(&w, rs->uk4);
		}
		if (rs->version >= 9 && rs->version < 14)
			buffer_write_int32(&w, rs->next_seq);
		buffer_write_int32(&w, rs->nr_heap_objs);
		for (int i = 0; i < rs->nr_heap_objs; i++) {
			enum rsave_heap_tag *tag = rs->heap[i];
			switch (*tag) {
			case RSAVE_GLOBALS:
			case RSAVE_LOCALS:
				write_heap_frame(&w, *tag, rs->version, rs->heap[i]);
				break;
			case RSAVE_STRING:
				write_heap_string(&w, rs->version, rs->heap[i]);
				break;
			case RSAVE_ARRAY:
				write_heap_array(&w, rs->version, rs->heap[i]);
				break;
			case RSAVE_STRUCT:
				write_heap_struct(&w, rs->version, rs->heap[i]);
				break;
			case RSAVE_DELEGATE:
				write_heap_delegate(&w, rs->version, rs->heap[i]);
				break;
			case RSAVE_NULL:
				buffer_write_int32(&w, -1);
				break;
			default:
				ERROR("unknown rsave heap tag %d", *tag);
			}
		}
		if (rs->version >= 6 && rs->version < 14) {
			buffer_write_int32(&w, rs->nr_func_names);
			for (int i = 0; i < rs->nr_func_names; i++)
				buffer_write_cstringz(&w, rs->func_names[i]);
		}
	}

	struct savefile save = {
		.buf = w.buf,
		.len = w.index,
		.encrypted = encrypt,
		.compression_level = compression_level
	};
	enum savefile_error err = savefile_write(&save, out);
	free(w.buf);
	return err;
}
