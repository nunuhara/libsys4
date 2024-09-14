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
		mt19937_xorcode(buf, compressed_size, GD11_ENCRYPT_KEY);
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
	if (gs->struct_defs) {
		for (int32_t i = 0; i < gs->nr_struct_defs; i++) {
			struct gsave_struct_def *sd = &gs->struct_defs[i];
			free(sd->name);
			for (int32_t j = 0; j < sd->nr_fields; j++) {
				free(sd->fields[j].name);
			}
			free(sd->fields);
		}
		free(gs->struct_defs);
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
		return (0 <= val && val < gs->nr_strings) || val == GSAVE7_EMPTY_STRING;
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
	if (gs->version != 4 && gs->version != 5 && gs->version != 7)
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
		if (gs->version <= 5) {
			rec->type = buffer_read_int32(&r);
			rec->struct_name = strdup(buffer_skip_string(&r));
		} else {
			rec->struct_index = buffer_read_int32(&r);
			rec->type = rec->struct_index == -1 ? GSAVE_RECORD_GLOBALS : GSAVE_RECORD_STRUCT;
		}
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
		if (gs->version <= 5)
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
			if (gs->version >= 7)
				fa->type = buffer_read_int32(&r);
			fa->values = xcalloc(fa->nr_values, sizeof(struct gsave_array_value));
			for (int i = 0; i < fa->nr_values; i++) {
				int32_t value = buffer_read_int32(&r);
				enum ain_data_type type = gs->version >= 7 ? fa->type : buffer_read_int32(&r);
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
			kv->type = buffer_read_int32(&r);
			kv->value = buffer_read_int32(&r);
			kv->name = strdup(buffer_skip_string(&r));
			if (!gsave_validate_value(kv->value, kv->type, gs))
				return SAVEFILE_INVALID;
		} else {
			kv->value = buffer_read_int32(&r);
		}
	}

	// struct-defs
	if (gs->version >= 7) {
		gs->nr_struct_defs = buffer_read_int32(&r);
		gs->struct_defs = xcalloc(gs->nr_struct_defs, sizeof(struct gsave_struct_def));
		for (struct gsave_struct_def *sd = gs->struct_defs; sd < gs->struct_defs + gs->nr_struct_defs; sd++) {
			sd->name = strdup(buffer_skip_string(&r));
			sd->nr_fields = buffer_read_int32(&r);
			sd->fields = xcalloc(sd->nr_fields, sizeof(struct gsave_field_def));
			for (struct gsave_field_def *fd = sd->fields; fd < sd->fields + sd->nr_fields; fd++) {
				fd->type = buffer_read_int32(&r);
				fd->name = strdup(buffer_skip_string(&r));
			}
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
		sd->fields[i].type = st->members[i].type.data;
		sd->fields[i].name = strdup(st->members[i].name);
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
	free(s);
}

static void rsave_free_delegate(struct rsave_heap_delegate *d)
{
	free(d);
}

void rsave_free(struct rsave *rs)
{
	free(rs->key);
	for (int i = 0; i < rs->nr_comments; i++)
		free(rs->comments[i]);
	free(rs->comments);
	free(rs->ip.caller_func);
	free(rs->stack);
	free(rs->call_frames);
	for (int i = 0; i < rs->nr_return_records; i++)
		free(rs->return_records[i].caller_func);
	free(rs->return_records);
	for (int i = 0; i < rs->nr_heap_objs; i++) {
		enum rsave_heap_tag *tag = rs->heap[i];
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
	for (int i = 0; i < rs->nr_func_names; i++)
		free(rs->func_names[i]);
	free(rs->func_names);
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

static int32_t *parse_int_array(struct buffer *r, int *num)
{
	int n = buffer_read_int32(r);
	int32_t *buf = xcalloc(n, sizeof(int32_t));
	for (int i = 0; i < n; i++)
		buf[i] = buffer_read_int32(r);
	*num = n;
	return buf;
}

static char **parse_string_array(struct buffer *r, int *num)
{
	int n = buffer_read_int32(r);
	char **strs = xcalloc(n, sizeof(char*));
	for (int i = 0; i < n; i++)
		strs[i] = strdup(buffer_skip_string(r));
	*num = n;
	return strs;
}

static struct rsave_symbol parse_rsave_symbol(struct buffer *r, int32_t version)
{
	if (version == 4)
		return (struct rsave_symbol) { .id = buffer_read_int32(r) };
	return (struct rsave_symbol) { .name = strdup(buffer_skip_string(r)) };
}

static struct rsave_call_frame *parse_call_frames(struct buffer *r, int *num)
{
	int32_t nr_local_ptrs, nr_frame_types, nr_struct_ptrs;
	int32_t *local_ptrs = parse_int_array(r, &nr_local_ptrs);
	int32_t *frame_types = parse_int_array(r, &nr_frame_types);
	int32_t *struct_ptrs = parse_int_array(r, &nr_struct_ptrs);
	if (nr_local_ptrs != nr_frame_types)
		ERROR("unexpected number of local pointers");

	struct rsave_call_frame *frames = xcalloc(nr_local_ptrs, sizeof(struct rsave_call_frame));
	int32_t struct_ptr_index = 0;
	for (int i = 0; i < nr_local_ptrs; i++) {
		frames[i].type = frame_types[i];
		frames[i].local_ptr = local_ptrs[i];
		frames[i].struct_ptr = frame_types[i] == RSAVE_METHOD_CALL ?
			struct_ptrs[struct_ptr_index++] : -1;
	}
	if (struct_ptr_index != nr_struct_ptrs)
		ERROR("unexpected number of struct pointers");
	free(local_ptrs);
	free(frame_types);
	free(struct_ptrs);
	*num = nr_local_ptrs;
	return frames;
}

static void parse_return_record(struct buffer *r, struct rsave_return_record *f)
{
	f->return_addr = buffer_read_int32(r);
	if (f->return_addr == -1)
		return;
	f->caller_func = strdup(buffer_skip_string(r));
	f->local_addr = buffer_read_int32(r);
	f->crc = buffer_read_int32(r);
}

static struct rsave_heap_frame *parse_heap_frame(struct buffer *r, int32_t version, enum rsave_heap_tag tag)
{
	struct rsave_heap_frame f = { .tag = tag };
	f.ref = buffer_read_int32(r);
	if (version >= 9)
		f.seq = buffer_read_int32(r);
	if (version == 4) {
		f.func.id = buffer_read_int32(r);
	} else if (tag == RSAVE_GLOBALS) {
		f.func.id = buffer_read_int32(r);
		if (f.func.id != -1)
			return NULL;
	} else {
		f.func.name = strdup(buffer_skip_string(r));
	}

	f.types = parse_int_array(r, &f.nr_types);
	if (tag == RSAVE_LOCALS && version >= 9)
		f.struct_ptr = buffer_read_int32(r);
	int slots_size = buffer_read_int32(r);
	if (slots_size % sizeof(int32_t) != 0) {
		free(f.func.name);
		free(f.types);
		return NULL;
	}
	f.nr_slots = slots_size / sizeof(int32_t);

	struct rsave_heap_frame *obj = xmalloc(sizeof(struct rsave_heap_frame) + slots_size);
	*obj = f;
	for (int i = 0; i < f.nr_slots; i++)
		obj->slots[i] = buffer_read_int32(r);
	return obj;
}

static struct rsave_heap_string *parse_heap_string(struct buffer *r, int32_t version)
{
	struct rsave_heap_string s = { .tag = RSAVE_STRING };
	s.ref = buffer_read_int32(r);
	if (version >= 9)
		s.seq = buffer_read_int32(r);
	s.uk = buffer_read_int32(r);
	if (s.uk != 0 && s.uk != 1)
		ERROR("unexpected");
	s.len = buffer_read_int32(r);
	struct rsave_heap_string *obj = xmalloc(sizeof(struct rsave_heap_string) + s.len);
	*obj = s;
	buffer_read_bytes(r, (uint8_t *)obj->text, s.len);
	return obj;
}

static struct rsave_heap_array *parse_heap_array(struct buffer *r, int32_t version)
{
	struct rsave_heap_array a = { .tag = RSAVE_ARRAY };
	a.ref = buffer_read_int32(r);
	if (version >= 9)
		a.seq = buffer_read_int32(r);
	a.rank_minus_1 = buffer_read_int32(r);
	a.data_type = buffer_read_int32(r);
	a.struct_type = parse_rsave_symbol(r, version);
	a.root_rank = buffer_read_int32(r);
	a.is_not_empty = buffer_read_int32(r);

	int slots_size = buffer_read_int32(r);
	if (slots_size % sizeof(int32_t) != 0) {
		free(a.struct_type.name);
		return NULL;
	}
	a.nr_slots = slots_size / sizeof(int32_t);
	struct rsave_heap_array *obj = xmalloc(sizeof(struct rsave_heap_array) + slots_size);
	*obj = a;
	for (int i = 0; i < a.nr_slots; i++)
		obj->slots[i] = buffer_read_int32(r);
	return obj;
}

static struct rsave_heap_struct *parse_heap_struct(struct buffer *r, int32_t version)
{
	struct rsave_heap_struct s = { .tag = RSAVE_STRUCT };
	s.ref = buffer_read_int32(r);
	if (version >= 9)
		s.seq = buffer_read_int32(r);
	s.ctor = parse_rsave_symbol(r, version);
	s.dtor = parse_rsave_symbol(r, version);
	s.uk = buffer_read_int32(r);
	if (s.uk != 0)
		ERROR("unexpected");
	s.struct_type = parse_rsave_symbol(r, version);
	s.types = parse_int_array(r, &s.nr_types);
	int slots_size = buffer_read_int32(r);
	if (slots_size % sizeof(int32_t) != 0) {
		free(s.ctor.name);
		free(s.dtor.name);
		free(s.struct_type.name);
		free(s.types);
		return NULL;
	}
	s.nr_slots = slots_size / sizeof(int32_t);
	struct rsave_heap_struct *obj = xmalloc(sizeof(struct rsave_heap_struct) + slots_size);
	*obj = s;
	for (int i = 0; i < s.nr_slots; i++)
		obj->slots[i] = buffer_read_int32(r);
	return obj;
}

static struct rsave_heap_delegate *parse_heap_delegate(struct buffer *r, int32_t version)
{
	if (version < 9)
		return NULL;
	struct rsave_heap_delegate d = { .tag = RSAVE_DELEGATE };
	d.ref = buffer_read_int32(r);
	d.seq = buffer_read_int32(r);
	int slots_size = buffer_read_int32(r);
	if (slots_size % sizeof(int32_t) != 0) {
		return NULL;
	}
	d.nr_slots = slots_size / sizeof(int32_t);
	struct rsave_heap_delegate *obj = xmalloc(sizeof(struct rsave_heap_delegate) + slots_size);
	*obj = d;
	for (int i = 0; i < d.nr_slots; i++)
		obj->slots[i] = buffer_read_int32(r);
	return obj;
}

enum savefile_error rsave_parse(uint8_t *buf, size_t len, enum rsave_read_mode mode, struct rsave *rs)
{
	struct buffer r;
	buffer_init(&r, buf, len);

	if (strcmp(buffer_strdata(&r), "RSM"))
		return SAVEFILE_INVALID_SIGNATURE;
	buffer_skip(&r, 4);

	rs->version = buffer_read_int32(&r);
	if (rs->version != 4 && rs->version != 6 && rs->version != 7 && rs->version != 9)
		return SAVEFILE_UNSUPPORTED_FORMAT;
	rs->key = strdup(buffer_skip_string(&r));
	if (rs->version >= 7) {
		rs->comments = parse_string_array(&r, &rs->nr_comments);
		if (buffer_remaining(&r) == 0) {
			rs->comments_only = true;
			return SAVEFILE_SUCCESS;
		}
	}

	if (mode == RSAVE_READ_COMMENTS) {
		rs->comments_only = true;
		return SAVEFILE_SUCCESS;
	}

	parse_return_record(&r, &rs->ip);
	rs->uk1 = buffer_read_int32(&r);
	if (rs->uk1)
		ERROR("unexpected");
	rs->stack = parse_int_array(&r, &rs->stack_size);
	rs->call_frames = parse_call_frames(&r, &rs->nr_call_frames);
	rs->nr_return_records = buffer_read_int32(&r);
	rs->return_records = xcalloc(rs->nr_return_records, sizeof(struct rsave_return_record));
	for (int i = 0; i < rs->nr_return_records; i++)
		parse_return_record(&r, &rs->return_records[i]);

	rs->uk2 = buffer_read_int32(&r);
	rs->uk3 = buffer_read_int32(&r);
	rs->uk4 = buffer_read_int32(&r);
	if (rs->version >= 9)
		rs->next_seq = buffer_read_int32(&r);
	if (rs->uk2 || rs->uk3 || rs->uk4)
		ERROR("unexpected");

	rs->nr_heap_objs = buffer_read_int32(&r);
	rs->heap = xcalloc(rs->nr_heap_objs, sizeof(void*));
	for (int i = 0; i < rs->nr_heap_objs; i++) {
		enum rsave_heap_tag tag = buffer_read_int32(&r);
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

	if (rs->version >= 6)
		rs->func_names = parse_string_array(&r, &rs->nr_func_names);

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
	if (version >= 9)
		buffer_write_int32(w, f->seq);
	write_rsave_symbol(w, &f->func);
	buffer_write_int32(w, f->nr_types);
	for (int i = 0; i < f->nr_types; i++)
		buffer_write_int32(w, f->types[i]);
	if (tag == RSAVE_LOCALS && version >= 9)
		buffer_write_int32(w, f->struct_ptr);
	buffer_write_int32(w, f->nr_slots * sizeof(int32_t));
	for (int i = 0; i < f->nr_slots; i++)
		buffer_write_int32(w, f->slots[i]);
}

static void write_heap_string(struct buffer *w, int32_t version, struct rsave_heap_string *s)
{
	buffer_write_int32(w, s->tag);
	buffer_write_int32(w, s->ref);
	if (version >= 9)
		buffer_write_int32(w, s->seq);
	buffer_write_int32(w, s->uk);
	buffer_write_int32(w, s->len);
	buffer_write_bytes(w, (uint8_t*)s->text, s->len);
}

static void write_heap_array(struct buffer *w, int32_t version, struct rsave_heap_array *a)
{
	buffer_write_int32(w, a->tag);
	buffer_write_int32(w, a->ref);
	if (version >= 9)
		buffer_write_int32(w, a->seq);
	buffer_write_int32(w, a->rank_minus_1);
	buffer_write_int32(w, a->data_type);
	write_rsave_symbol(w, &a->struct_type);
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
	if (version >= 9)
		buffer_write_int32(w, s->seq);
	write_rsave_symbol(w, &s->ctor);
	write_rsave_symbol(w, &s->dtor);
	buffer_write_int32(w, s->uk);
	write_rsave_symbol(w, &s->struct_type);
	buffer_write_int32(w, s->nr_types);
	for (int i = 0; i < s->nr_types; i++)
		buffer_write_int32(w, s->types[i]);
	buffer_write_int32(w, s->nr_slots * sizeof(int32_t));
	for (int i = 0; i < s->nr_slots; i++)
		buffer_write_int32(w, s->slots[i]);
}

static void write_heap_delegate(struct buffer *w, int32_t version, struct rsave_heap_delegate *d)
{
	buffer_write_int32(w, d->tag);
	buffer_write_int32(w, d->ref);
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
		buffer_write_int32(&w, rs->uk3);
		buffer_write_int32(&w, rs->uk4);
		if (rs->version >= 9)
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
		if (rs->version >= 6) {
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
