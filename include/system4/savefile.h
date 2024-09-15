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

#ifndef SYSTEM4_SAVEFILE_H
#define SYSTEM4_SAVEFILE_H

#include <stdint.h>
#include <stdio.h>
#include "system4/ain.h"

#define GSAVE7_EMPTY_STRING 0x7fffffff

struct string;

enum savefile_error {
	SAVEFILE_SUCCESS,
	SAVEFILE_FILE_ERROR,
	SAVEFILE_INVALID_SIGNATURE,
	SAVEFILE_UNSUPPORTED_FORMAT,
	SAVEFILE_INVALID,
	SAVEFILE_INTERNAL_ERROR,
	SAVEFILE_MAX_ERROR
};

struct savefile {
	uint8_t *buf;
	size_t len;
	bool encrypted;
	int compression_level;
};

const char *savefile_strerror(enum savefile_error error);
void savefile_free(struct savefile *save);
struct savefile *savefile_read(const char *path, enum savefile_error *error);
enum savefile_error savefile_write(struct savefile *save, FILE *out);

// Save data structure of system.GlobalSave / system.GroupSave
struct gsave {
	char *key;
	int32_t uk1;  // always 1000?
	int32_t version;
	int32_t uk2;  // always 0x38 (56)?
	int32_t nr_ain_globals;
	char *group;  // version 5+

	int32_t nr_records;
	int32_t cap_records;
	struct gsave_record *records;
	int32_t nr_globals;
	struct gsave_global *globals;
	int32_t nr_strings;
	int32_t cap_strings;
	struct string **strings;
	int32_t nr_arrays;
	int32_t cap_arrays;
	struct gsave_array *arrays;
	int32_t nr_keyvals;
	int32_t cap_keyvals;
	struct gsave_keyval *keyvals;
	// version 7+
	int32_t nr_struct_defs;
	int32_t cap_struct_defs;
	struct gsave_struct_def *struct_defs;
};

enum gsave_record_type {
	GSAVE_RECORD_STRUCT = AIN_STRUCT,
	GSAVE_RECORD_GLOBALS = 1000,
};

struct gsave_record {
	enum gsave_record_type type;  // version <=5
	char *struct_name;  // version <=5
	int32_t struct_index;  // -1 for globals record. version 7+
	int32_t nr_indices;
	int32_t *indices;
};

struct gsave_global {
	enum ain_data_type type;
	int32_t value;
	char *name;
	int32_t unknown;  // version <=5, always 1?
};

struct gsave_array {
	int rank;  // -1 for unalocated array
	int32_t *dimensions;  // in reversed order
	int32_t nr_flat_arrays;
	struct gsave_flat_array *flat_arrays;
};

struct gsave_flat_array {
	int32_t nr_values;
	enum ain_data_type type;  // version 7+
	struct gsave_array_value *values;
};

struct gsave_array_value {
	int32_t value;
	enum ain_data_type type;  // version <=5
};

struct gsave_keyval {
	enum ain_data_type type;  // version <=5
	int32_t value;
	char *name;  // version <=5
};

struct gsave_struct_def {
	char *name;
	int32_t nr_fields;
	struct gsave_field_def *fields;
};

struct gsave_field_def {
	enum ain_data_type type;
	char *name;
};

struct gsave *gsave_create(int version, const char *key, int nr_ain_globals, const char *group);
void gsave_free(struct gsave *gs);
struct gsave *gsave_read(const char *path, enum savefile_error *error);
enum savefile_error gsave_parse(uint8_t *buf, size_t len, struct gsave *gs);
enum savefile_error gsave_write(struct gsave *gs, FILE *out, bool encrypt, int compression_level);

int32_t gsave_add_globals_record(struct gsave *gs, int nr_globals);
int32_t gsave_add_record(struct gsave *gs, struct gsave_record *rec);
int32_t gsave_add_string(struct gsave *gs, struct string *s);
int32_t gsave_add_array(struct gsave *gs, struct gsave_array *array);
int32_t gsave_add_keyval(struct gsave *gs, struct gsave_keyval *kv);
int32_t gsave_add_struct_def(struct gsave *gs, struct ain_struct *st);
int32_t gsave_get_struct_def(struct gsave *gs, const char *name);

struct rsave_return_record {
	int32_t return_addr;  // -1 for the dummy record at the callstack bottom
	char *caller_func;
	int32_t local_addr;  // offset from the function address
	int32_t crc;
};

// Save data structure of system.ResumeSave
struct rsave {
	int32_t version;
	char *key;
	int32_t nr_comments;  // version 7+
	char **comments;
	bool comments_only;  // if true, the following fields are not used
	struct rsave_return_record ip;
	int32_t uk1;  // always zero
	int32_t stack_size;
	int32_t *stack;
	int32_t nr_call_frames;
	struct rsave_call_frame *call_frames;
	int32_t nr_return_records;
	struct rsave_return_record *return_records;
	int32_t uk2;  // always zero
	int32_t uk3;  // always zero
	int32_t uk4;  // always zero
	int32_t next_seq;  // version 9+
	int32_t nr_heap_objs;
	void **heap;  // pointers to `struct rsave_heap_xxx` or rsave_null
	int32_t nr_func_names;  // version 6+
	char **func_names;
};

enum rsave_frame_type {
	RSAVE_ENTRY_POINT = 0,
	RSAVE_FUNCTION_CALL = 1,
	RSAVE_METHOD_CALL = 2,
	RSAVE_CALL_STACK_BOTTOM = 4,
};

struct rsave_call_frame {
	enum rsave_frame_type type;
	int32_t local_ptr;
	int32_t struct_ptr;  // used if type == RSAVE_METHOD_CALL
};

struct rsave_symbol {
	char *name;  // version 6+
	int32_t id;  // version 4
};

enum rsave_heap_tag {
	RSAVE_GLOBALS = 0,
	RSAVE_LOCALS = 1,
	RSAVE_STRING = 2,
	RSAVE_ARRAY = 3,
	RSAVE_STRUCT = 4,
	RSAVE_DELEGATE = 5,  // version 9+
	RSAVE_NULL = -1
};

struct rsave_heap_frame {
	enum rsave_heap_tag tag;  // RSAVE_GLOBALS or RSAVE_LOCALS
	int32_t ref;
	int32_t seq;  // version 9+
	struct rsave_symbol func;
	int32_t nr_types;
	int32_t *types;
	int32_t struct_ptr;  // only in RSAVE_LOCALS, version 9+
	int32_t nr_slots;
	int32_t slots[];
};

struct rsave_heap_string {
	enum rsave_heap_tag tag;  // RSAVE_STRING
	int32_t ref;
	int32_t seq;  // version 9+
	int32_t uk;   // 0 or (very rarely) 1
	int32_t len;  // including null terminator
	char text[];
};

struct rsave_heap_array {
	enum rsave_heap_tag tag;  // RSAVE_ARRAY
	int32_t ref;
	int32_t seq;  // version 9+
	int32_t rank_minus_1;  // -1 if null
	enum ain_data_type data_type;
	struct rsave_symbol struct_type;
	int32_t root_rank;  // -1 if null
	// This is usually `nr_slots > 0 ? 1 : 0`, but is 1 when the size is reduced
	// to 0 by Array.Erase().
	int32_t is_not_empty;
	int32_t nr_slots;
	int32_t slots[];
};

struct rsave_heap_struct {
	enum rsave_heap_tag tag;  // RSAVE_STRUCT
	int32_t ref;
	int32_t seq;  // version 9+
	struct rsave_symbol ctor;
	struct rsave_symbol dtor;
	int32_t uk;   // always zero?
	struct rsave_symbol struct_type;
	int32_t nr_types;
	int32_t *types;
	int32_t nr_slots;
	int32_t slots[];
};

struct rsave_heap_delegate {
	enum rsave_heap_tag tag;  // RSAVE_DELEGATE
	int32_t ref;
	int32_t seq;
	int32_t nr_slots;
	int32_t slots[];
};

struct rsave_heap_null {
	enum rsave_heap_tag tag;  // RSAVE_NULL
};
extern struct rsave_heap_null * const rsave_null;

enum rsave_read_mode {
	RSAVE_READ_ALL,
	RSAVE_READ_COMMENTS,
};

void rsave_free(struct rsave *rs);
struct rsave *rsave_read(const char *path, enum rsave_read_mode mode, enum savefile_error *error);
enum savefile_error rsave_parse(uint8_t *buf, size_t len, enum rsave_read_mode mode, struct rsave *rs);
enum savefile_error rsave_write(struct rsave *rs, FILE *out, bool encrypt, int compression_level);

#endif /* SYSTEM4_SAVEFILE_H */
