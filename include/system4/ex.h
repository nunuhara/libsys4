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

#ifndef SYSTEM4_EX_H
#define SYSTEM4_EX_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

enum ex_value_type {
	EX_INT = 1,
	EX_FLOAT = 2,
	EX_STRING = 3,
	EX_TABLE = 4,
	EX_LIST = 5,
	EX_TREE = 6
};

struct ex_value {
	enum ex_value_type type;
	union {
		int32_t i;
		float f;
		struct string *s;
		struct ex_table *t;
		struct ex_list *list;
		struct ex_tree *tree;
	};
	int id; // for MainEXFile handles
};

struct ex_field {
	enum ex_value_type type;
	struct string *name;
	int32_t has_value;
	struct ex_value value;
	int32_t is_index;
	uint32_t nr_subfields;
	struct ex_field *subfields;
};

struct ex_table {
	uint32_t nr_fields;
	struct ex_field *fields;
	uint32_t nr_columns;
	uint32_t nr_rows;
	struct ex_value **rows;
};

struct ex_list_item {
	uint32_t size;
	struct ex_value value;
};

struct ex_list {
	uint32_t nr_items;
	struct ex_list_item *items;
};

struct ex_leaf {
	uint32_t size;
	struct string *name;
	struct ex_value value;
};

struct ex_tree {
	struct string *name;
	bool is_leaf;
	union {
		struct {
			uint32_t nr_children;
			struct ex_value *_children;
			struct ex_tree *children;
		};
		struct ex_leaf leaf;
	};
};

struct ex_block {
	size_t size;
	struct string *name;
	struct ex_value val;
};

struct ex {
	uint32_t nr_blocks;
	struct ex_block *blocks;
};

uint8_t *ex_decrypt(const char *path, size_t *size, uint32_t *nr_blocks);
struct ex *ex_read(const uint8_t *data, size_t size);
struct ex *ex_read_conv(const uint8_t *data, size_t size, struct string*(*conv)(const char*,size_t));
struct ex *ex_read_file(const char *path);
struct ex *ex_read_file_conv(const char *path, struct string*(*conv)(const char*,size_t));
void ex_append(struct ex *base, struct ex *append);
struct ex *ex_extract_append(struct ex *base, struct ex *append);
void ex_replace(struct ex *base, struct ex *replace);
void ex_free(struct ex *ex);

void ex_encode(uint8_t *buf, size_t size);
const char *ex_strtype(enum ex_value_type type);

struct ex_value *ex_get(struct ex *ex, const char *name);
int32_t ex_get_int(struct ex *ex, const char *name, int32_t dflt);
float ex_get_float(struct ex *ex, const char *name, float dflt);
struct string *ex_get_string(struct ex *ex, const char *name);
struct ex_table *ex_get_table(struct ex *ex, const char *name);
struct ex_list *ex_get_list(struct ex *ex, const char *name);
struct ex_tree *ex_get_tree(struct ex *ex, const char *name);

struct ex_value *ex_table_get(struct ex_table *table, unsigned row, unsigned col);
struct ex_value *ex_list_get(struct ex_list *list, unsigned i);
struct ex_tree *ex_tree_get_child(struct ex_tree *tree, const char *name);
struct ex_value *ex_leaf_value(struct ex_tree *tree);

int ex_row_at_int_key(struct ex_table *t, int key);
int ex_row_at_string_key(struct ex_table *t, const char *key);
int ex_col_from_name(struct ex_table *t, const char *name);

#endif /* SYSTE4_EX_H */
