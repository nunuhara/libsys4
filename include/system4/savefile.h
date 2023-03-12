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
	struct gsave_record *records;
	int32_t nr_globals;
	struct gsave_global *globals;
	int32_t nr_strings;
	struct string **strings;
	int32_t nr_arrays;
	struct gsave_array *arrays;
	int32_t nr_keyvals;
	struct gsave_keyval *keyvals;
};

enum gsave_record_type {
	GSAVE_RECORD_STRUCT = AIN_STRUCT,
	GSAVE_RECORD_GLOBALS = 1000,
};

struct gsave_record {
	enum gsave_record_type type;
	char *struct_name;
	int32_t nr_indices;
	int32_t *indices;
};

struct gsave_global {
	enum ain_data_type type;
	int32_t value;
	char *name;
	int32_t unknown;  // always 1 (removed in gsave v7, so probably unused)
};

struct gsave_array {
	int rank;  // -1 for unalocated array
	int32_t *dimensions;  // in reversed order
	int32_t nr_flat_arrays;
	struct gsave_flat_array *flat_arrays;
};

struct gsave_flat_array {
	int32_t nr_values;
	struct gsave_array_value *values;
};

struct gsave_array_value {
	int32_t value;
	enum ain_data_type type;
};

struct gsave_keyval {
	enum ain_data_type type;
	int32_t value;
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

#endif /* SYSTEM4_SAVEFILE_H */
