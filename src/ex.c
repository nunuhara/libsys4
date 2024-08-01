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
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include <zlib.h>
#include "little_endian.h"
#include "system4.h"
#include "system4/buffer.h"
#include "system4/ex.h"
#include "system4/file.h"
#include "system4/string.h"

#define _EX_ERROR(buf, fmt, ...) ERROR("At 0x%08x: " fmt, (uint32_t)(buf)->index, ##__VA_ARGS__)
#define EX_ERROR(reader, fmt, ...) ERROR("At 0x%08x: " fmt, (uint32_t)(reader)->buf.index, ##__VA_ARGS__)
#define EX_WARNING(reader, fmt, ...) WARNING("At 0x%08x: " fmt, (uint32_t)(reader)->buf.index, ##__VA_ARGS__)

struct ex_reader {
	struct buffer buf;
	struct string *(*conv)(const char*, size_t);
};

static struct ex_block *ex_get_block(struct ex *ex, const char *name, enum ex_value_type type);

const char *ex_strtype(enum ex_value_type type)
{
	switch (type) {
	case EX_INT: return "int";
	case EX_FLOAT: return "float";
	case EX_STRING: return "string";
	case EX_TABLE: return "table";
	case EX_LIST: return "list";
	case EX_TREE: return "tree";
	default: return "unknown_type";
	}
}

uint8_t ex_decode_table[256];
uint8_t ex_decode_table_inv[256];

static struct string *ex_read_pascal_string(struct ex_reader *r)
{
	struct string *s = buffer_conv_pascal_string(&r->buf, r->conv);
	if (!s)
		EX_ERROR(r, "Failed to read string");
	s->size = strlen(s->text);
	return s;
}

static struct string *ex_read_string(struct ex_reader *r)
{
	struct string *s = ex_read_pascal_string(r);
	// TODO: at most 3 bytes padding, no need for full strlen
	s->size = strlen(s->text);
	// TODO: validate?
	return s;
}

static bool ex_initialized = false;

static void ex_init(void)
{
	// initialize table
	for (int i = 0; i < 256; i++) {
		int tmpfor = i;
		int tmp = tmpfor;
		tmp = (tmp & 0x55) + ((tmp >> 1) & 0x55);
		tmp = (tmp & 0x33) + ((tmp >> 2) & 0x33);
		tmp = (tmp & 0x0F) + ((tmp >> 4) & 0x0F);
		if ((tmp & 0x01) == 0) {
			tmpfor = ((tmpfor << (8 - tmp)) | (tmpfor >> tmp)) & 0xFF;
		} else {
			tmpfor = ((tmpfor >> (8 - tmp)) | (tmpfor << tmp)) & 0xFF;
		}
		ex_decode_table[i] = tmpfor;
	}
	// initialize inverse table
	for (int i = 0; i < 256; i++) {
		ex_decode_table_inv[ex_decode_table[i]] = i;
	}

	ex_initialized = true;
}

void ex_encode(uint8_t *buf, size_t size)
{
	if (!ex_initialized)
		ex_init();

	for (size_t i = 0; i < size; i++) {
		buf[i] = ex_decode_table_inv[buf[i]];
	}
}

static uint8_t *ex_decode(uint8_t *data, size_t *len, uint32_t *nr_blocks)
{
	struct buffer r;
	uint32_t compressed_size;
	unsigned long uncompressed_size;

	if (!ex_initialized)
		ex_init();

	buffer_init(&r, data, *len);
	if (strncmp(buffer_strdata(&r), "HEAD", 4))
		_EX_ERROR(&r, "Missing HEAD section marker");
	buffer_skip(&r, 8);

	if (strncmp(buffer_strdata(&r), "EXTF", 4)) {
		_EX_ERROR(&r, "Missing EXTF section marker");
	}
	buffer_skip(&r, 8);

	if (nr_blocks)
		*nr_blocks = buffer_read_int32(&r);
	else
		buffer_read_int32(&r);

	if (strncmp(buffer_strdata(&r), "DATA", 4)) {
		_EX_ERROR(&r, "Missing DATA section marker");
	}
	buffer_skip(&r, 4);

	compressed_size = buffer_read_int32(&r);
	uncompressed_size = buffer_read_int32(&r);

	// decode compressed data
	for (size_t i = 0; i < compressed_size; i++) {
		data[r.index+i] = ex_decode_table[data[r.index+i]];
	}

	uint8_t *out = xmalloc(uncompressed_size);
	int rv = uncompress(out, &uncompressed_size, (uint8_t*)buffer_strdata(&r), compressed_size);
	switch (rv) {
	case Z_BUF_ERROR:  ERROR("Uncompress failed: Z_BUF_ERROR");
	case Z_MEM_ERROR:  ERROR("Uncompress failed: Z_MEM_ERROR");
	case Z_DATA_ERROR: ERROR("Uncompress failed: Z_DATA_ERROR");
	case Z_OK:         break;
	default:           ERROR("Uncompress failed: %d", rv);
	}

	// decompress
	*len = uncompressed_size;
	return out;
}

static void ex_read_fields(struct ex_reader *r, struct ex_table *table);
static void ex_read_table(struct ex_reader *r, struct ex_table *table, struct ex_field *fields, uint32_t nr_fields);
static void ex_read_list(struct ex_reader *r, struct ex_list *list);

static void _ex_read_value(struct ex_reader *r, struct ex_value *value, struct ex_field *fields, uint32_t nr_fields)
{
	switch (value->type) {
	case EX_INT:
		value->i = buffer_read_int32(&r->buf);
		break;
	case EX_FLOAT:
		value->f = buffer_read_float(&r->buf);
		break;
	case EX_STRING:
		value->s = ex_read_pascal_string(r);
		break;
	case EX_TABLE:
		value->t = xcalloc(1, sizeof(struct ex_table));
		// XXX: if nr_fields is zero, we are NOT a sub-table and therefore need to read fields
		if (!nr_fields) {
			ex_read_fields(r, value->t);
			ex_read_table(r, value->t, value->t->fields, value->t->nr_fields);
		} else {
			ex_read_table(r, value->t, fields, nr_fields);
		}
		break;
	case EX_LIST:
		value->list = xcalloc(1, sizeof(struct ex_list));
		ex_read_list(r, value->list);
		break;
	default:
		EX_ERROR(r, "Unhandled value type: %d", value->type);
	}
}

static void ex_read_value(struct ex_reader *r, struct ex_value *value, struct ex_field *fields, uint32_t nr_fields)
{
	value->type = buffer_read_int32(&r->buf);
	_ex_read_value(r, value, fields, nr_fields);
}

static void ex_read_field(struct ex_reader *r, struct ex_field *field)
{
	field->type = buffer_read_int32(&r->buf);
	if (field->type < EX_INT || field->type > EX_TABLE)
		EX_ERROR(r, "Unknown/invalid field type: %d", field->type);

	field->name = ex_read_string(r);
	field->has_value = buffer_read_int32(&r->buf);
	field->is_index = buffer_read_int32(&r->buf);
	if (field->has_value) {
		field->value.type = field->type;
		_ex_read_value(r, &field->value, NULL, 0);
	}
	if (field->has_value && field->has_value != 1)
		EX_WARNING(r, "Non-boolean for field->has_value: %d", field->has_value);
	if (field->is_index && field->is_index != 1)
		EX_WARNING(r, "Non-boolean for field->is_index: %d", field->is_index);

	if (field->type == EX_TABLE) {
		field->nr_subfields = buffer_read_int32(&r->buf);
		if (field->nr_subfields > 255)
			EX_ERROR(r, "Too many subfields: %u", field->nr_subfields);

		field->subfields = xcalloc(field->nr_subfields, sizeof(struct ex_field));
		for (uint32_t i = 0; i < field->nr_subfields; i++) {
			ex_read_field(r, &field->subfields[i]);
		}
	}
}

static void ex_read_fields(struct ex_reader *r, struct ex_table *table)
{
	table->nr_fields = buffer_read_int32(&r->buf);
	table->fields = xcalloc(table->nr_fields, sizeof(struct ex_field));
	for (uint32_t i = 0; i < table->nr_fields; i++) {
		ex_read_field(r, &table->fields[i]);
	}
}

enum table_layout {
	TABLE_DEFAULT,
	TABLE_COLUMNS_FIRST,
	TABLE_ROWS_FIRST
};

static enum table_layout table_layout = TABLE_DEFAULT;

static void ex_read_table(struct ex_reader *r, struct ex_table *table, struct ex_field *fields, uint32_t nr_fields)
{
	// NOTE: Starting in Evenicle, the rows/columns quantities are reversed
	if (table_layout == TABLE_ROWS_FIRST) {
		table->nr_rows = buffer_read_int32(&r->buf);
		table->nr_columns = buffer_read_int32(&r->buf);
	} else {
		table->nr_columns = buffer_read_int32(&r->buf);
		table->nr_rows = buffer_read_int32(&r->buf);
	}

	// try switching to column-major order if fields don't make sense
	if (table_layout == TABLE_DEFAULT && table->nr_columns != nr_fields) {
		if (table->nr_rows == nr_fields) {
			uint32_t tmp = table->nr_columns;
			table->nr_columns = table->nr_rows;
			table->nr_rows = tmp;
			table_layout = TABLE_ROWS_FIRST;
		} else {
			EX_ERROR(r, "Number of fields doesn't match number of columns: %u, %u", table->nr_columns, nr_fields);
		}
	} else if (table->nr_columns != nr_fields) {
		EX_ERROR(r, "Number of fields doesn't match number of columns: %u, %u", table->nr_columns, nr_fields);
	}

	table->rows = xcalloc(table->nr_rows, sizeof(struct ex_value*));
	for (uint32_t i = 0; i < table->nr_rows; i++) {
		table->rows[i] = xcalloc(table->nr_columns, sizeof(struct ex_value));
		for (uint32_t j = 0; j < table->nr_columns; j++) {
			ex_read_value(r, &table->rows[i][j], fields[j].subfields, fields[j].nr_subfields);
			if (table->rows[i][j].type != fields[j].type) {
				// broken table in Rance 03?
				EX_WARNING(r, "Column type doesn't match field type: expected %s; got %s",
					   ex_strtype(fields[j].type), ex_strtype(table->rows[i][j].type));
			}
		}
	}
}

static void ex_read_list(struct ex_reader *r, struct ex_list *list)
{
	list->nr_items = buffer_read_int32(&r->buf);
	list->items = xcalloc(list->nr_items, sizeof(struct ex_list_item));
	for (uint32_t i = 0; i < list->nr_items; i++) {
		list->items[i].value.type = buffer_read_int32(&r->buf);
		list->items[i].size = buffer_read_int32(&r->buf);
		size_t data_loc = r->buf.index;
		_ex_read_value(r, &list->items[i].value, NULL, 0);
		if (r->buf.index - data_loc != list->items[i].size) {
			EX_ERROR(r, "Incorrect size for list item: %zu / %zu",
				 list->items[i].size, r->buf.index - data_loc);
		}
	}
}

static void ex_read_tree(struct ex_reader *r, struct ex_tree *tree)
{
	tree->name = ex_read_string(r);
	uint32_t is_leaf = buffer_read_int32(&r->buf);
	if (is_leaf > 1)
		EX_ERROR(r, "tree->is_leaf is not a boolean: %u", is_leaf);
	tree->is_leaf = is_leaf;

	if (!tree->is_leaf) {
		tree->nr_children = buffer_read_int32(&r->buf);
		tree->_children = xcalloc(tree->nr_children, sizeof(struct ex_value));
		tree->children = xcalloc(tree->nr_children, sizeof(struct ex_tree));
		for (uint32_t i = 0; i < tree->nr_children; i++) {
			tree->_children[i].type = EX_TREE;
			tree->_children[i].tree = &tree->children[i];
			ex_read_tree(r, &tree->children[i]);
		}
	} else {
		tree->leaf.value.type = buffer_read_int32(&r->buf);
		tree->leaf.size = buffer_read_int32(&r->buf);
		size_t data_loc = r->buf.index;
		tree->leaf.name = ex_read_string(r);
		_ex_read_value(r, &tree->leaf.value, NULL, 0);

		if (r->buf.index - data_loc != tree->leaf.size) {
			EX_ERROR(r, "Incorrect size for leaf node: %zu / %zu",
				 tree->leaf.size, r->buf.index - data_loc);
		}

		int32_t zero = buffer_read_int32(&r->buf);
		if (zero) {
			EX_ERROR(r, "Expected 0 after leaf node: 0x%x at 0x%zx", zero, r->buf.index);
		}
	}
}

static void ex_read_block(struct ex_reader *r, struct ex_block *block)
{
	block->val.type = buffer_read_int32(&r->buf);
	if (block->val.type < EX_INT || block->val.type > EX_TREE)
		EX_ERROR(r, "Unknown/invalid block type: %d", block->val.type);

	block->size = buffer_read_int32(&r->buf);
	if (block->size > buffer_remaining(&r->buf))
		EX_ERROR(r, "Block size extends past end of file: %zu", block->size);

	size_t data_loc = r->buf.index;
	block->name = ex_read_string(r);

	switch (block->val.type) {
	case EX_INT:
		block->val.i = buffer_read_int32(&r->buf);
		break;
	case EX_FLOAT:
		block->val.f = buffer_read_float(&r->buf);
		break;
	case EX_STRING:
		block->val.s = ex_read_pascal_string(r);
		break;
	case EX_TABLE:
		block->val.t = xcalloc(1, sizeof(struct ex_table));
		ex_read_fields(r, block->val.t);
		ex_read_table(r, block->val.t, block->val.t->fields, block->val.t->nr_fields);
		break;
	case EX_LIST:
		block->val.list = xcalloc(1, sizeof(struct ex_list));
		ex_read_list(r, block->val.list);
		break;
	case EX_TREE:
		block->val.tree = xcalloc(1, sizeof(struct ex_tree));
		ex_read_tree(r, block->val.tree);
		break;
	}

	if (r->buf.index - data_loc != block->size) {
		EX_ERROR(r, "Incorrect block size: %zu / %zu",
			 r->buf.index - data_loc, block->size);
	}
}

uint8_t *ex_decrypt(const char *path, size_t *size, uint32_t *nr_blocks)
{
	uint8_t *file_data = file_read(path, size);
	uint8_t *decoded = ex_decode(file_data, size, nr_blocks);
	free(file_data);
	return decoded;
}

static struct ex *_ex_read(uint8_t *data, size_t size, struct string*(*conv)(const char*,size_t))
{
	uint32_t nr_blocks;
	uint8_t *decoded;
	struct ex *ex;

	struct ex_reader r = {
		.conv = conv,
	};

	decoded = ex_decode(data, &size, &nr_blocks);
	buffer_init(&r.buf, decoded, size);

	ex = xmalloc(sizeof(struct ex));
	ex->nr_blocks = nr_blocks;
	ex->blocks = xcalloc(nr_blocks, sizeof(struct ex_block));
	for (size_t i = 0; i < nr_blocks; i++) {
		ex_read_block(&r, &ex->blocks[i]);
	}

	free(decoded);
	return ex;
}

struct ex *ex_read_conv(const uint8_t *data, size_t size, struct string*(*conv)(const char*,size_t))
{
	uint8_t *copy = xmalloc(size);
	memcpy(copy, data, size);
	struct ex *ex = _ex_read(copy, size, conv);
	free(copy);
	return ex;
}

struct ex *ex_read(const uint8_t *data, size_t size)
{
	return ex_read_conv(data, size, make_string);
}

struct ex *ex_read_file_conv(const char *path, struct string*(*conv)(const char*,size_t))
{
	size_t size;
	uint8_t *data = file_read(path, &size);
	if (!data)
		return NULL;
	struct ex *ex = ex_read_conv(data, size, conv);
	free(data);
	return ex;
}

struct ex *ex_read_file(const char *path)
{
	return ex_read_file_conv(path, make_string);
}

static void ex_copy_value(struct ex_value *out, struct ex_value *in);

static struct ex_field *ex_copy_fields(struct ex_field *fields, unsigned nr_fields)
{
	struct ex_field *out = xcalloc(nr_fields, sizeof(struct ex_field));
	for (unsigned i = 0; i < nr_fields; i++) {
		out[i].type = fields[i].type;
		out[i].name = string_ref(fields[i].name);
		out[i].has_value = fields[i].has_value;
		if (fields[i].has_value) {
			ex_copy_value(&out[i].value, &fields[i].value);
		}
		out[i].is_index = fields[i].is_index;
		out[i].nr_subfields = fields[i].nr_subfields;
		if (fields[i].nr_subfields) {
			out[i].subfields = ex_copy_fields(fields[i].subfields, fields[i].nr_subfields);
		}
	}
	return out;
}

static struct ex_value *ex_copy_values(struct ex_value *values, unsigned nr_values)
{
	struct ex_value *out = xcalloc(nr_values, sizeof(struct ex_value));
	for (unsigned i = 0; i < nr_values; i++) {
		ex_copy_value(&out[i], &values[i]);
	}
	return out;
}

static struct ex_table *ex_copy_table(struct ex_table *t)
{
	struct ex_table *out = xmalloc(sizeof(struct ex_table));
	out->nr_fields = t->nr_fields;
	out->fields = ex_copy_fields(t->fields, t->nr_fields);
	out->nr_columns = t->nr_columns;
	out->nr_rows = t->nr_rows;
	out->rows = xcalloc(t->nr_rows, sizeof(struct ex_value*));
	for (unsigned i = 0; i < t->nr_rows; i++) {
		out->rows[i] = ex_copy_values(t->rows[i], t->nr_columns);
	}
	return out;
}

static struct ex_list *ex_copy_list(struct ex_list *list)
{
	struct ex_list *out = xmalloc(sizeof(struct ex_list));
	out->nr_items = list->nr_items;
	out->items = xcalloc(list->nr_items, sizeof(struct ex_list_item));
	for (unsigned i = 0; i < list->nr_items; i++) {
		out->items[i].size = list->items[i].size;
		ex_copy_value(&out->items[i].value, &list->items[i].value);
	}
	return out;
}

static void _ex_copy_tree(struct ex_tree *out, struct ex_tree *tree)
{
	out->name = string_ref(tree->name);
	out->is_leaf = tree->is_leaf;
	if (tree->is_leaf) {
		out->leaf.size = tree->leaf.size;
		out->leaf.name = string_ref(tree->leaf.name);
		ex_copy_value(&out->leaf.value, &tree->leaf.value);
		return;
	}

	out->nr_children = tree->nr_children;
	out->_children = xcalloc(tree->nr_children, sizeof(struct ex_value));
	out->children = xcalloc(tree->nr_children, sizeof(struct ex_tree));
	for (unsigned i = 0; i < tree->nr_children; i++) {
		out->_children[i].type = EX_TREE;
		out->_children[i].tree = &out->children[i];
		_ex_copy_tree(&out->children[i], &tree->children[i]);
	}
}

static struct ex_tree *ex_copy_tree(struct ex_tree *tree)
{
	struct ex_tree *out = xmalloc(sizeof(struct ex_tree));
	_ex_copy_tree(out, tree);
	return out;
}

static void ex_copy_value(struct ex_value *out, struct ex_value *in)
{
	out->type = in->type;
	out->id = in->id;
	switch (in->type) {
	case EX_INT:    out->i = in->i; break;
	case EX_FLOAT:  out->f = in->f; break;
	case EX_STRING: out->s = string_ref(in->s); break;
	case EX_TABLE:  out->t = ex_copy_table(in->t); break;
	case EX_LIST:   out->list = ex_copy_list(in->list); break;
	case EX_TREE:   out->tree = ex_copy_tree(in->tree); break;
	}
}

static void ex_copy_block(struct ex_block *out, struct ex_block *in)
{
	out->size = in->size;
	out->name = string_ref(in->name);
	ex_copy_value(&out->val, &in->val);
}

static bool ex_value_equal(struct ex_value *a, struct ex_value *b)
{
	if (a->type != b->type)
		return false;
	switch (a->type) {
	case EX_INT:    return a->i == b->i;
	case EX_FLOAT:  return fabs(a->f - b->f) < 0.00001;
	case EX_STRING: return !strcmp(a->s->text, b->s->text);
	default:        return false;
	}
}

static bool ex_field_equal(struct ex_field *a, struct ex_field *b)
{
	if (a->type != b->type)
		return false;
	if (strcmp(a->name->text, b->name->text))
		return false;
	if (a->has_value != b->has_value)
		return false;
	if (a->has_value && !ex_value_equal(&a->value, &b->value))
		return false;
	if (a->is_index != b->is_index)
		return false;
	if (a->nr_subfields != b->nr_subfields)
		return false;
	for (unsigned i = 0; i < a->nr_subfields; i++) {
		if (!ex_field_equal(&a->subfields[i], &b->subfields[i]))
			return false;
	}
	return true;
}

static bool ex_header_equal(struct ex_table *a, struct ex_table *b)
{
	if (a->nr_columns != b->nr_columns)
		return false;
	for (unsigned i = 0; i < a->nr_columns; i++) {
		if (!ex_field_equal(&a->fields[i], &b->fields[i]))
			return false;
	}
	return true;
}

static void ex_append_table(struct ex_table *out, struct ex_table *in)
{
	if (!ex_header_equal(out, in))
		ERROR("Table headers do not match");

	// FIXME: should check if key exists in table and update rather than append
	out->rows = xrealloc_array(out->rows, out->nr_rows, out->nr_rows+in->nr_rows, sizeof(struct ex_value*));
	for (unsigned i = 0; i < in->nr_rows; i++) {
		out->rows[out->nr_rows+i] = ex_copy_values(in->rows[i], in->nr_columns);
	}
	out->nr_rows += in->nr_rows;
}

static void ex_append_list(struct ex_list *out, struct ex_list *in)
{
	out->items = xrealloc_array(out->items, out->nr_items, out->nr_items+in->nr_items, sizeof(struct ex_list_item));
	for (unsigned i = 0; i < in->nr_items; i++) {
		out->items[out->nr_items+i].size = in->items[i].size;
		ex_copy_value(&out->items[out->nr_items+i].value, &in->items[i].value);
	}
	out->nr_items += in->nr_items;
}

static void ex_append_tree(struct ex_tree *out, struct ex_tree *in);
static void ex_free_value(struct ex_value *value);

static void ex_tree_append_child(struct ex_tree *out, struct ex_tree *child)
{
	// look for a child node with the same name, and update if one exists
	for (unsigned i = 0; i < out->nr_children; i++) {
		struct ex_tree *out_child = &out->children[i];
		if (!strcmp(child->name->text, out_child->name->text)) {
			if (child->is_leaf != out_child->is_leaf)
				ERROR("Tree nodes with same name have different type");
			if (child->is_leaf) {
				ex_free_value(&out_child->leaf.value);
				ex_copy_value(&out_child->leaf.value, &child->leaf.value);
			} else {
				ex_append_tree(out_child, child);
			}
			return;
		}
	}

	// no child with same name, append new child
	out->_children = xrealloc_array(out->_children, out->nr_children, out->nr_children+1, sizeof(struct ex_value));
	out->children = xrealloc_array(out->children, out->nr_children, out->nr_children+1, sizeof(struct ex_tree));
	out->_children[out->nr_children].type = EX_TREE;
	out->_children[out->nr_children].tree = &out->children[out->nr_children];
	_ex_copy_tree(&out->children[out->nr_children], child);
	out->nr_children++;
}

static void ex_append_tree(struct ex_tree *out, struct ex_tree *in)
{
	if (out->is_leaf || in->is_leaf)
		ERROR("Tried to append to leaf node");

	for (unsigned i = 0; i < in->nr_children; i++) {
		ex_tree_append_child(out, &in->children[i]);
	}
}

/*
 * Append data from `append` to data in `base`, and return an ex object
 * containing only the objects added/modified in `append`.
 */
struct ex *ex_extract_append(struct ex *base, struct ex *append)
{
	struct ex *out = xmalloc(sizeof(struct ex));
	out->nr_blocks = 0;
	out->blocks = NULL;

	for (unsigned i = 0; i < append->nr_blocks; i++) {
		struct ex_block *src = ex_get_block(base, append->blocks[i].name->text, append->blocks[i].val.type);
		out->blocks = xrealloc_array(out->blocks, out->nr_blocks, out->nr_blocks+1, sizeof(struct ex_block));
		if (src) {
			switch (src->val.type) {
			case EX_INT:
			case EX_FLOAT:
			case EX_STRING:
				ex_copy_block(&out->blocks[out->nr_blocks], &append->blocks[i]);
				break;
			case EX_TABLE:
				ex_copy_block(&out->blocks[out->nr_blocks], src);
				ex_append_table(out->blocks[out->nr_blocks].val.t, append->blocks[i].val.t);
				break;
			case EX_LIST:
				ex_copy_block(&out->blocks[out->nr_blocks], src);
				ex_append_list(out->blocks[out->nr_blocks].val.list, append->blocks[i].val.list);
				break;
			case EX_TREE:
				ex_copy_block(&out->blocks[out->nr_blocks], src);
				ex_append_tree(out->blocks[out->nr_blocks].val.tree, append->blocks[i].val.tree);
				break;
			}
		} else {
			ex_copy_block(&out->blocks[out->nr_blocks], &append->blocks[i]);
		}
		out->nr_blocks++;
	}
	return out;
}

void ex_append(struct ex *base, struct ex *append)
{
	for (unsigned i = 0; i < append->nr_blocks; i++) {
		struct ex_block *src = ex_get_block(base, append->blocks[i].name->text, append->blocks[i].val.type);
		if (src) {
			switch (src->val.type) {
			case EX_INT:    src->val.i = append->blocks[i].val.i; break;
			case EX_FLOAT:  src->val.f = append->blocks[i].val.f; break;
			case EX_STRING: src->val.s = string_ref(append->blocks[i].val.s); break;
			case EX_TABLE:  ex_append_table(src->val.t, append->blocks[i].val.t); break;
			case EX_LIST:   ex_append_list(src->val.list, append->blocks[i].val.list); break;
			case EX_TREE:   ex_append_tree(src->val.tree, append->blocks[i].val.tree); break;
			}
		} else {
			base->blocks = xrealloc_array(base->blocks, base->nr_blocks, base->nr_blocks+1, sizeof(struct ex_block));
			ex_copy_block(&base->blocks[base->nr_blocks], &append->blocks[i]);
			base->nr_blocks++;
		}
	}
}

void ex_replace(struct ex *base, struct ex *replace)
{
	for (unsigned i = 0; i < replace->nr_blocks; i++) {
		struct ex_block *src = ex_get_block(base, replace->blocks[i].name->text, replace->blocks[i].val.type);
		if (src) {
			ex_free_value(&src->val);
			src->size = replace->blocks[i].size;
			ex_copy_value(&src->val, &replace->blocks[i].val);
		} else {
			base->blocks = xrealloc_array(base->blocks, base->nr_blocks, base->nr_blocks+1, sizeof(struct ex_block));
			ex_copy_block(&base->blocks[base->nr_blocks], &replace->blocks[i]);
			base->nr_blocks++;
		}
	}
}

static void ex_free_table(struct ex_table *table);
static void ex_free_list(struct ex_list *list);
static void ex_free_tree(struct ex_tree *tree);

static void ex_free_value(struct ex_value *value)
{
	switch (value->type) {
	case EX_STRING:
		free_string(value->s);
		break;
	case EX_TABLE:
		ex_free_table(value->t);
		break;
	case EX_LIST:
		ex_free_list(value->list);
		break;
	case EX_TREE:
		ex_free_tree(value->tree);
		free(value->tree);
		break;
	default:
		break;
	}
}

static void ex_free_values(struct ex_value *values, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++) {
		ex_free_value(&values[i]);
	}
	free(values);
}

static void ex_free_fields(struct ex_field *fields, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++) {
		free_string(fields[i].name);
		if (fields[i].type == EX_STRING && fields[i].has_value)
			free_string(fields[i].value.s);
		ex_free_fields(fields[i].subfields, fields[i].nr_subfields);
	}
	free(fields);
}

static void ex_free_table(struct ex_table *table)
{
	ex_free_fields(table->fields, table->nr_fields);
	for (uint32_t i = 0; i < table->nr_rows; i++) {
		ex_free_values(table->rows[i], table->nr_columns);
	}
	free(table->rows);
	free(table);
}

static void ex_free_list(struct ex_list *list)
{
	for (uint32_t i = 0; i < list->nr_items; i++) {
		ex_free_value(&list->items[i].value);
	}
	free(list->items);
	free(list);
}

static void ex_free_tree(struct ex_tree *tree)
{
	free_string(tree->name);

	if (tree->is_leaf) {
		free_string(tree->leaf.name);
		ex_free_value(&tree->leaf.value);
		return;
	}

	for (uint32_t i = 0; i < tree->nr_children; i++) {
		ex_free_tree(&tree->children[i]);
	}
	free(tree->_children);
	free(tree->children);
}

void ex_free(struct ex *ex)
{
	for (uint32_t i = 0; i < ex->nr_blocks; i++) {
		struct ex_block *block = &ex->blocks[i];
		free_string(block->name);
		switch (block->val.type) {
		case EX_STRING:
			free_string(block->val.s);
			break;
		case EX_TABLE:
			ex_free_table(block->val.t);
			break;
		case EX_LIST:
			ex_free_list(block->val.list);
			break;
		case EX_TREE:
			ex_free_tree(block->val.tree);
			free(block->val.tree);
			break;
		default:
			break;
		}
	}
	free(ex->blocks);
	free(ex);
}

static struct ex_value *ex_tree_get_path(struct ex_tree *tree, const char *path)
{
	char *next = strchr(path, '.');
	size_t len = next ? (size_t)(next - path) : strlen(path);

	if (tree->is_leaf) {
		if (!next && !strcmp(tree->leaf.name->text, path))
			return &tree->leaf.value;
		return NULL;
	}

	for (unsigned i = 0; i < tree->nr_children; i++) {
		if (!strncmp(tree->children[i].name->text, path, len)) {
			if (next) {
				return ex_tree_get_path(&tree->children[i], next+1);
			}
			if (tree->children[i].is_leaf)
				return &tree->children[i].leaf.value;
			return &tree->_children[i];
		}
	}

	return NULL;
}

static struct ex_value *_ex_get(struct ex *ex, const char *name, size_t name_len)
{
	// TODO: use hash table
	for (unsigned i = 0; i < ex->nr_blocks; i++) {
		if (!strncmp(ex->blocks[i].name->text, name, name_len))
			return &ex->blocks[i].val;
	}
	return NULL;
}

struct ex_value *ex_get(struct ex *ex, const char *name)
{
	char *next = strchr(name, '.');
	size_t len = next ? (size_t)(next - name) : strlen(name);
	struct ex_value *v = _ex_get(ex, name, len);
	if (!v)
		return NULL;
	if (!next)
		return v;
	if (v->type != EX_TREE)
		return NULL;
	return ex_tree_get_path(v->tree, next+1);
}

static struct ex_block *ex_get_block(struct ex *ex, const char *name, enum ex_value_type type)
{
	for (unsigned i = 0; i < ex->nr_blocks; i++) {
		if (ex->blocks[i].val.type != type)
			continue;
		if (!strcmp(ex->blocks[i].name->text, name))
			return &ex->blocks[i];
	}
	return NULL;
}

int32_t ex_get_int(struct ex *ex, const char *name, int32_t dflt)
{
	struct ex_block *b = ex_get_block(ex, name, EX_INT);
	if (!b)
		return dflt;
	return b->val.i;
}

float ex_get_float(struct ex *ex, const char *name, float dflt)
{
	struct ex_block *b = ex_get_block(ex, name, EX_FLOAT);
	if (!b)
		return dflt;
	return b->val.f;
}

struct string *ex_get_string(struct ex *ex, const char *name)
{
	struct ex_block *b = ex_get_block(ex, name, EX_STRING);
	if (!b)
		return NULL;
	return string_ref(b->val.s);
}

struct ex_table *ex_get_table(struct ex *ex, const char *name)
{
	struct ex_block *b = ex_get_block(ex, name, EX_TABLE);
	if (!b)
		return NULL;
	return b->val.t;
}

struct ex_list *ex_get_list(struct ex *ex, const char *name)
{
	struct ex_block *b = ex_get_block(ex, name, EX_LIST);;
	if (!b)
		return NULL;
	return b->val.list;
}

struct ex_tree *ex_get_tree(struct ex *ex, const char *name)
{
	struct ex_block *b = ex_get_block(ex, name, EX_TREE);;
	if (!b)
		return NULL;
	return b->val.tree;
}

struct ex_value *ex_table_get(struct ex_table *table, unsigned row, unsigned col)
{
	if (row >= table->nr_rows || col >= table->nr_columns)
		return NULL;
	return &table->rows[row][col];
}

struct ex_value *ex_list_get(struct ex_list *list, unsigned i)
{
	if (i >= list->nr_items)
		return NULL;
	return &list->items[i].value;
}

struct ex_tree *ex_tree_get_child(struct ex_tree *tree, const char *name)
{
	if (tree->is_leaf)
		return NULL;
	for (unsigned i = 0; i < tree->nr_children; i++) {
		if (!strcmp(tree->children[i].name->text, name)) {
			return &tree->children[i];
		}
	}
	return NULL;
}

struct ex_value *ex_leaf_value(struct ex_tree *tree)
{
	if (!tree->is_leaf)
		return NULL;
	return &tree->leaf.value;
}

int ex_row_at_int_key(struct ex_table *t, int key)
{
	int col = -1;
	for (unsigned i = 0; i < t->nr_fields; i++) {
		if (t->fields[i].is_index) {
			col = i;
			break;
		}
	}
	if (col < 0)
		return -1;
	if (t->fields[col].type != EX_INT)
		return -1;

	// TODO: use hash table
	for (unsigned row = 0; row < t->nr_rows; row++) {
		if (t->rows[row][col].i == key) {
			return row;
		}
	}
	return -1;
}

int ex_row_at_string_key(struct ex_table *t, const char *key)
{
	int col = -1;
	for (unsigned i = 0; i < t->nr_fields; i++) {
		if (t->fields[i].is_index) {
			col = i;
			break;
		}
	}
	if (col < 0)
		return -1;
	if (t->fields[col].type != EX_STRING)
		return -1;

	// TODO: use hash table
	for (unsigned row = 0; row < t->nr_rows; row++) {
		if (!strcmp(t->rows[row][col].s->text, key)) {
			return row;
		}
	}
	return -1;
}

int ex_col_from_name(struct ex_table *t, const char *name)
{
	for (unsigned i = 0; i < t->nr_fields; i++) {
		if (!strcmp(t->fields[i].name->text, name))
			return i;
	}
	return -1;
}

