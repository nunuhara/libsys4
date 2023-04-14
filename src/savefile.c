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
#include <zlib.h>

#include "little_endian.h"
#include "system4.h"
#include "system4/buffer.h"
#include "system4/file.h"
#include "system4/mt19937int.h"
#include "system4/savefile.h"
#include "system4/string.h"

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
	free(save->buf);
	free(save);
}

static void savefile_decrypt(uint8_t *buf, size_t size)
{
	struct mt19937 mt;
	mt19937_init(&mt, 0x12320f);
	for (size_t i = 0; i < size; i++)
		buf[i] ^= mt19937_genrand(&mt);
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
	fseek(fp, 0, SEEK_END);
	long compressed_size = ftell(fp) - sizeof(header);
	fseek(fp, sizeof(header), SEEK_SET);
	if (compressed_size < 2) {
		*error = SAVEFILE_INVALID;
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
		savefile_decrypt(buf, compressed_size);
	}
	switch (buf[1]) {
	case 0x01: save->compression_level = Z_BEST_SPEED; break;
	case 0xda: save->compression_level = Z_BEST_COMPRESSION; break;
	default:   save->compression_level = Z_DEFAULT_COMPRESSION; break;
	}

	unsigned long raw_size = LittleEndian_getDW(header, 4);
	save->buf = xmalloc(raw_size);
	if (uncompress(save->buf, &raw_size, buf, compressed_size) != Z_OK) {
		*error = SAVEFILE_INVALID;
		goto err;
	}
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
	unsigned long bufsize = compressBound(save->len);
	uint8_t *buf = xmalloc(bufsize);
	int r = compress2(buf, &bufsize, save->buf, save->len, save->compression_level);
	if (r != Z_OK) {
		free(buf);
		return SAVEFILE_INTERNAL_ERROR;
	}

	if (save->encrypted)
		savefile_decrypt(buf, bufsize);

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
	gs->uk2 = 56;
	gs->nr_ain_globals = nr_ain_globals;
	if (version >= 5)
		gs->group = strdup(group ? group : "");
	return gs;
}

void gsave_free(struct gsave *gs)
{
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
			for (int32_t j = 0; j < gs->arrays[i].nr_flat_arrays; j++) {
				free(gs->arrays[i].flat_arrays[j].values);
			}
			free(gs->arrays[i].flat_arrays);
		}
		free(gs->arrays);
	}
	if (gs->keyvals) {
		for (int32_t i = 0; i < gs->nr_keyvals; i++) {
			free(gs->keyvals[i].name);
		}
		free(gs->keyvals);
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
	case AIN_REF_TYPE:
		return true;
	case AIN_STRING:
		return 0 <= val && val < gs->nr_strings;
	case AIN_STRUCT:
		return 0 <= val && val < gs->nr_records;
	case AIN_ARRAY_TYPE:
		return 0 <= val && val < gs->nr_arrays;
	default:
		return false;
	}
}

enum savefile_error gsave_parse(uint8_t *buf, size_t len, struct gsave *gs)
{
	struct buffer r;
	buffer_init(&r, buf, len);

	gs->key = strdup(buffer_skip_string(&r));
	gs->uk1 = buffer_read_int32(&r);
	gs->version = buffer_read_int32(&r);
	if (gs->version != 4 && gs->version != 5)
		return SAVEFILE_UNSUPPORTED_FORMAT;
	gs->uk2 = buffer_read_int32(&r);
	gs->nr_ain_globals = buffer_read_int32(&r);

	size_t records_offset = buffer_read_int32(&r);
	gs->nr_records = buffer_read_int32(&r);
	size_t globals_offset = buffer_read_int32(&r);
	gs->nr_globals = buffer_read_int32(&r);
	size_t strings_offset = buffer_read_int32(&r);
	gs->nr_strings = buffer_read_int32(&r);
	size_t arrays_offset = buffer_read_int32(&r);
	gs->nr_arrays = buffer_read_int32(&r);
	size_t keyvals_offset = buffer_read_int32(&r);
	gs->nr_keyvals = buffer_read_int32(&r);

	if (gs->version >= 5) {
		gs->group = strdup(buffer_skip_string(&r));
	}

	// records
	if (r.index != records_offset)
		return SAVEFILE_INVALID;
	gs->records = xcalloc(gs->nr_records, sizeof(struct gsave_record));
	for (struct gsave_record *rec = gs->records; rec < gs->records + gs->nr_records; rec++) {
		rec->type = buffer_read_int32(&r);
		rec->struct_name = strdup(buffer_skip_string(&r));
		rec->nr_indices = buffer_read_int32(&r);
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
			int32_t index = buffer_read_int32(&r);
			if (index < 0 || index >= index_ubound)
				return SAVEFILE_INVALID;
			rec->indices[i] = index;
		}
	}

	// globals
	if (r.index != globals_offset)
		return SAVEFILE_INVALID;
	gs->globals = xcalloc(gs->nr_globals, sizeof(struct gsave_global));
	for (struct gsave_global *g = gs->globals; g < gs->globals + gs->nr_globals; g++) {
		g->type = buffer_read_int32(&r);
		g->value = buffer_read_int32(&r);
		g->name = strdup(buffer_skip_string(&r));
		g->unknown = buffer_read_int32(&r);
		if (!gsave_validate_value(g->value, g->type, gs))
			return SAVEFILE_INVALID;
	}

	// strings
	if (r.index != strings_offset)
		return SAVEFILE_INVALID;
	gs->strings = xcalloc(gs->nr_strings, sizeof(struct string *));
	for (int i = 0; i < gs->nr_strings; i++) {
		gs->strings[i] = buffer_read_string(&r);
	}

	// arrays
	if (r.index != arrays_offset)
		return SAVEFILE_INVALID;
	gs->arrays = xcalloc(gs->nr_arrays, sizeof(struct gsave_array));
	for (struct gsave_array *a = gs->arrays; a < gs->arrays + gs->nr_arrays; a++) {
		a->rank = buffer_read_int32(&r);
		int expected_nr_flat_arrays = 0;
		if (a->rank > 0) {
			expected_nr_flat_arrays = 1;
			a->dimensions = xcalloc(a->rank, sizeof(int32_t));
			for (int i = 0; i < a->rank; i++) {
				a->dimensions[i] = buffer_read_int32(&r);
				if (i != 0)
					expected_nr_flat_arrays *= a->dimensions[i];
			}
		}
		a->nr_flat_arrays = buffer_read_int32(&r);
		if (a->nr_flat_arrays != expected_nr_flat_arrays)
			return SAVEFILE_INVALID;
		a->flat_arrays = xcalloc(a->nr_flat_arrays, sizeof(struct gsave_flat_array));
		for (struct gsave_flat_array *fa = a->flat_arrays; fa < a->flat_arrays + a->nr_flat_arrays; fa++) {
			fa->nr_values = buffer_read_int32(&r);
			if (fa->nr_values != a->dimensions[0])
				return SAVEFILE_INVALID;
			fa->values = xcalloc(fa->nr_values, sizeof(struct gsave_array_value));
			for (int i = 0; i < fa->nr_values; i++) {
				int32_t value = buffer_read_int32(&r);
				enum ain_data_type type = buffer_read_int32(&r);
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
		kv->type = buffer_read_int32(&r);
		kv->value = buffer_read_int32(&r);
		kv->name = strdup(buffer_skip_string(&r));
		if (!gsave_validate_value(kv->value, kv->type, gs))
			return SAVEFILE_INVALID;
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

	if (gs->version >= 5)
		buffer_write_cstringz(&w, gs->group);

	// records
	buffer_write_int32_at(&w, records_offset_loc, w.index);
	for (struct gsave_record *r = gs->records; r < gs->records + gs->nr_records; r++) {
		buffer_write_int32(&w, r->type);
		buffer_write_cstringz(&w, r->struct_name);
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
			for (int i = 0; i < fa->nr_values; i++) {
				buffer_write_int32(&w, fa->values[i].value);
				buffer_write_int32(&w, fa->values[i].type);
			}
		}
	}

	// key-values
	buffer_write_int32_at(&w, keyvals_offset_loc, w.index);
	for (struct gsave_keyval *kv = gs->keyvals; kv < gs->keyvals + gs->nr_keyvals; kv++) {
		buffer_write_int32(&w, kv->type);
		buffer_write_int32(&w, kv->value);
		buffer_write_cstringz(&w, kv->name);
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
		.type = GSAVE_RECORD_GLOBALS,
		.struct_name = strdup(""),
		.nr_indices = nr_globals,
		.indices = xcalloc(nr_globals, sizeof(int32_t)),
	};
	for (int i = 0; i < nr_globals; i++)
		rec.indices[i] = i;
	gsave_add_record(gs, &rec);
	gs->nr_globals = nr_globals;
	gs->globals = xcalloc(nr_globals, sizeof(struct gsave_global));
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
