/* Copyright (C) 2019 Nunuhara Cabbage <nunuhara@haniwa.technology>
 *
 * Credit to SLC for reverse engineering AIN formats.
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

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <libgen.h>
#include <assert.h>
#include <zlib.h>

#include "little_endian.h"
#include "system4.h"
#include "system4/ain.h"
#include "system4/file.h"
#include "system4/hashtable.h"
#include "system4/instructions.h"
#include "system4/mt19937int.h"
#include "system4/string.h"

struct func_list {
	int nr_slots;
	int slots[];
};
#define func_list_size(nr_slots) (sizeof(struct func_list) + sizeof(int)*(nr_slots))

static void func_ht_add(struct ain *ain, int i)
{
	struct ht_slot *kv = ht_put(ain->_func_ht, ain->functions[i].name, NULL);
	if (kv->value) {
		// entry with this name already exists; add to list
		struct func_list *list = kv->value;
		list = xrealloc(list, func_list_size(list->nr_slots+1));
		list->slots[list->nr_slots] = i;
		list->nr_slots++;
		kv->value = list;
	} else {
		// empty bucket: create list
		struct func_list *list = xmalloc(func_list_size(1));
		list->nr_slots = 1;
		list->slots[0] = i;
		kv->value = list;
	}
}

void ain_index_functions(struct ain *ain)
{
	if (ain->_func_ht) {
		ht_foreach_value(ain->_func_ht, free);
		ht_free(ain->_func_ht);
	}
	ain->_func_ht = ht_create(1024);
	for (int i = 0; i < ain->nr_functions; i++) {
		func_ht_add(ain, i);
	}
}

static void struct_ht_add(struct ain *ain, intptr_t i)
{
	struct ain_struct *s = &ain->structures[i];
	struct ht_slot *kv = ht_put(ain->_struct_ht, s->name, NULL);
	if (kv->value) {
		WARNING("Duplicate structure names: '%s'", s->name);
	}
	kv->value = (void*)i;
}

void ain_index_structures(struct ain *ain)
{
	if (ain->_struct_ht) {
		ht_free(ain->_struct_ht);
	}
	ain->_struct_ht = ht_create(1024);
	for (int i = 0; i < ain->nr_structures; i++) {
		struct_ht_add(ain, i);
	}
}

static intptr_t string_ht_get(struct ain *ain, const char *str)
{
	return (intptr_t)ht_get(ain->_string_ht, str, (void*)-1);
}

static intptr_t string_ht_add(struct ain *ain, const char *str, intptr_t i)
{
	struct ht_slot *kv = ht_put(ain->_string_ht, str, (void*)-1);
	if ((intptr_t)kv->value >= 0) {
		return (intptr_t)kv->value;
	}
	kv->value = (void*)i;
	return i;
}

static void init_string_ht(struct ain *ain)
{
	ain->_string_ht = ht_create(1024);
	for (int i = 0; i < ain->nr_strings; i++) {
		if (string_ht_add(ain, ain->strings[i]->text, i) != i) {
			WARNING("Duplicate string in string table");
		}
	}
}

static bool function_is_member_of(char *func_name, char *struct_name)
{
	while (*func_name && *struct_name && *func_name != '@' && *func_name == *struct_name) {
		func_name++;
		struct_name++;
	}
	return !*struct_name && *func_name == '@';
}

/*
 * Infer struct member functions from function names.
 */
void ain_init_member_functions(struct ain *ain, char *(*to_ascii)(const char*))
{
	// XXX: we convert all struct names up front to avoid repeated conversions below
	char **struct_names = xcalloc(ain->nr_structures, sizeof(char*));
	for (int i = 0; i < ain->nr_structures; i++) {
		struct_names[i] = to_ascii(ain->structures[i].name);
	}
	char **enum_names = xcalloc(ain->nr_enums, sizeof(char*));
	for (int i = 0; i < ain->nr_enums; i++) {
		enum_names[i] = to_ascii(ain->enums[i].name);
	}

	for (int f = 0; f < ain->nr_functions; f++) {
		ain->functions[f].struct_type = -1;
		ain->functions[f].enum_type = -1;
		char *name = to_ascii(ain->functions[f].name);
		if (!strchr(name, '@')) {
			free(name);
			continue;
		}
		for (int s = 0; s < ain->nr_structures; s++) {
			if (function_is_member_of(name, struct_names[s])) {
				ain->functions[f].struct_type = s;
				break;
			}
		}
		if (ain->functions[f].struct_type != -1) {
			free(name);
			continue;
		}
		// check enums
		for (int e = 0; e < ain->nr_enums; e++) {
			if (function_is_member_of(name, enum_names[e])) {
				ain->functions[f].enum_type = e;
				break;
			}
		}
		if (ain->functions[f].enum_type == -1)
			WARNING("Failed to find struct type for function \"%s\"", name);
		free(name);
	}
	for (int i = 0; i < ain->nr_structures; i++) {
		free(struct_names[i]);
	}
	free(struct_names);
	for (int i = 0; i < ain->nr_enums; i++) {
		free(enum_names[i]);
	}
	free(enum_names);
}

static struct func_list *get_function(struct ain *ain, const char *name)
{
	return ht_get(ain->_func_ht, name, NULL);
}

int ain_get_function(struct ain *ain, char *name)
{
	size_t len;
	long n = 0;

	// handle name#index syntax
	for (len = 0; name[len]; len++) {
		if (name[len] == '#') {
			char *endptr;
			n = strtol(name+len+1, &endptr, 10);
			if (!name[len+1] || *endptr || n < 0) {
				WARNING("Invalid function name: '%s'", name);
				n = 0;
			}
			name[len] = '\0';
			break;
		}
	}

	struct func_list *funs = get_function(ain, name);
	if (!funs || n >= funs->nr_slots)
		return -1;
	return funs->slots[n];
}

int ain_get_function_index(struct ain *ain, struct ain_function *f)
{
	struct func_list *funs = get_function(ain, f->name);
	if (!funs)
		goto err;

	for (int i = 0; i < funs->nr_slots; i++) {
		if (&ain->functions[funs->slots[i]] == f)
			return i;
	}
err:
	WARNING("Invalid function: '%s'", f->name);
	return 0;
}

int ain_get_struct(struct ain *ain, char *name)
{
	return (intptr_t)ht_get(ain->_struct_ht, name, (void*)-1);
}

int ain_add_struct(struct ain *ain, const char *name)
{
	int no = ain->nr_structures;
	ain->structures = xrealloc_array(ain->structures, no, no+1, sizeof(struct ain_struct));
	ain->structures[no].name = strdup(name);
	ain->structures[no].constructor = -1;
	ain->structures[no].destructor = -1;
	struct_ht_add(ain, no);
	ain->nr_structures++;
	return no;
}

int ain_get_enum(struct ain *ain, char *name)
{
	for (int i = 0; i < ain->nr_enums; i++) {
		if (!strcmp(ain->enums[i].name, name))
			return i;
	}
	return -1;
}

int ain_add_global(struct ain *ain, const char *name)
{
	int no = ain->nr_globals;
	ain->globals = xrealloc_array(ain->globals, ain->nr_globals, ain->nr_globals+1, sizeof(struct ain_variable));
	ain->globals[no].name = strdup(name);
	if (AIN_VERSION_GTE(ain, 12, 0))
		ain->globals[no].name2 = strdup("");
	ain->globals[no].var_type = AIN_VAR_GLOBAL;
	ain->nr_globals++;
	return no;
}

int ain_get_global(struct ain *ain, const char *name)
{
	for (int i = 0; i < ain->nr_globals; i++) {
		if (!strcmp(ain->globals[i].name, name))
			return i;
	}
	return -1;
}

int ain_add_initval(struct ain *ain, int global_index)
{
	int no = ain->nr_initvals;
	ain->global_initvals = xrealloc_array(ain->global_initvals, no, no+1, sizeof(struct ain_initval));
	ain->global_initvals[no].global_index = global_index;
	ain->nr_initvals++;
	return no;
}

// FIXME: This doesn't work correctly on ascii-incompatible encodings where
//        the '@' character can occur in a multi-byte sequence.
static void function_init_struct_type(struct ain *ain, struct ain_function *f)
{
	f->struct_type = -1;
	f->enum_type = -1;

	char *at = strchr(f->name, '@');
	if (!at)
		return;

	size_t len = at - f->name;
	char *struct_name = xmalloc(len + 1);
	memcpy(struct_name, f->name, len);
	struct_name[len] = '\0';

	int struct_no = ain_get_struct(ain, struct_name);
	if (struct_no >= 0) {
		f->struct_type = struct_no;
	} else {
		for (int i = 0; i < ain->nr_enums; i++) {
			if (!strcmp(struct_name, ain->enums[i].name)) {
				f->enum_type = i;
				break;
			}
		}
	}

	free(struct_name);
}

int ain_add_function(struct ain *ain, const char *name)
{
	int no = ain->nr_functions;
	ain->functions = xrealloc_array(ain->functions, no, no+1, sizeof(struct ain_function));
	ain->functions[no].name = strdup(name);
	function_init_struct_type(ain, &ain->functions[no]);
	func_ht_add(ain, no);
	ain->nr_functions++;
	return no;
}

void ain_copy_type(struct ain_type *dst, const struct ain_type *src)
{
	*dst = *src;
	if (src->array_type) {
		dst->array_type = xcalloc(1, sizeof(struct ain_type));
		ain_copy_type(dst->array_type, src->array_type);
	}
}

int ain_dup_function(struct ain *ain, int src_no)
{
	int dst_no = ain->nr_functions;
	ain->functions = xrealloc_array(ain->functions, dst_no, dst_no+1, sizeof(struct ain_function));

	struct ain_function *src = &ain->functions[src_no];
	struct ain_function *dst = &ain->functions[dst_no];

	*dst = *src;
	dst->name = strdup(src->name);
	ain_copy_type(&dst->return_type, &src->return_type);
	dst->struct_type = src->struct_type;
	dst->enum_type = src->enum_type;
	dst->vars = xcalloc(src->nr_vars, sizeof(struct ain_variable));
	for (int i = 0; i < src->nr_vars; i++) {
		dst->vars[i] = src->vars[i];
		dst->vars[i].name = strdup(src->vars[i].name);
		if (src->vars[i].name2) {
			dst->vars[i].name2 = strdup(src->vars[i].name2);
		}
		ain_copy_type(&dst->vars[i].type, &src->vars[i].type);
	}
	func_ht_add(ain, dst_no);
	ain->nr_functions++;
	return dst_no;
}

int ain_add_functype(struct ain *ain, const char *name)
{
	int no = ain->nr_function_types;
	ain->function_types = xrealloc_array(ain->function_types, no, no+1, sizeof(struct ain_function_type));
	ain->function_types[no].name = strdup(name);
	ain->nr_function_types++;
	ain->FNCT.present = true;
	return no;
}

int ain_get_functype(struct ain *ain, const char *name)
{
	for (int i = 0; i < ain->nr_function_types; i++) {
		if (!strcmp(ain->function_types[i].name, name))
			return i;
	}
	return -1;
}

int ain_add_delegate(struct ain *ain, const char *name)
{
	int no = ain->nr_delegates;
	ain->delegates = xrealloc_array(ain->delegates, no, no+1, sizeof(struct ain_function_type));
	ain->delegates[no].name = strdup(name);
	ain->nr_delegates++;
	ain->DELG.present = true;
	return no;
}

int ain_get_delegate(struct ain *ain, const char *name)
{
	for (int i = 0; i < ain->nr_delegates; i++) {
		if (!strcmp(ain->delegates[i].name, name))
			return i;
	}
	return -1;
}

int ain_add_string(struct ain *ain, const char *str)
{
	if (!ain->_string_ht)
		init_string_ht(ain);
	int i = string_ht_add(ain, str, ain->nr_strings);
	if (i == ain->nr_strings) {
		ain->strings = xrealloc_array(ain->strings, ain->nr_strings, ain->nr_strings+1, sizeof(struct string*));
		ain->strings[ain->nr_strings++] = make_string(str, strlen(str));
	}
	return i;
}

int ain_get_string_no(struct ain *ain, const char *str)
{
	if (!ain->_string_ht)
		init_string_ht(ain);
	return string_ht_get(ain, str);
}

int ain_add_message(struct ain *ain, const char *str)
{
	ain->messages = xrealloc_array(ain->messages, ain->nr_messages, ain->nr_messages+1, sizeof(struct string*));
	ain->messages[ain->nr_messages++] = make_string(str, strlen(str));
	return ain->nr_messages - 1;
}

int ain_add_switch(struct ain *ain)
{
	int no = ain->nr_switches;
	ain->switches = xrealloc_array(ain->switches, no, no+1, sizeof(struct ain_switch));
	ain->switches[no].case_type = AIN_SWITCH_INT;
	ain->switches[no].default_address = -1;
	ain->nr_switches++;
	return no;
}

int ain_add_file(struct ain *ain, const char *filename)
{
	ain->filenames = xrealloc_array(ain->filenames, ain->nr_filenames, ain->nr_filenames+1, sizeof(char*));
	ain->filenames[ain->nr_filenames++] = strdup(filename);
	return ain->nr_filenames - 1;
}

int ain_add_library(struct ain *ain, const char *name)
{
	int no = ain->nr_libraries;
	ain->libraries = xrealloc_array(ain->libraries, no, no+1, sizeof(struct ain_library));
	ain->libraries[no].name = strdup(name);
	ain->nr_libraries++;
	return no;
}

int ain_get_library(struct ain *ain, const char *name)
{
	for (int i = 0; i < ain->nr_libraries; i++) {
		if (!strcmp(ain->libraries[i].name, name))
			return i;
	}
	return -1;
}

int ain_get_library_function(struct ain *ain, int libno, const char *name)
{
	assert(libno < ain->nr_libraries);
	struct ain_library *lib = &ain->libraries[libno];
	for (int i = 0; i < lib->nr_functions; i++) {
		if (!strcmp(lib->functions[i].name, name))
			return i;
	}
	return -1;
}

static const char *errtab[AIN_MAX_ERROR] = {
	[AIN_SUCCESS]             = "Success",
	[AIN_FILE_ERROR]          = "Error opening AIN file",
	[AIN_UNRECOGNIZED_FORMAT] = "Unrecognized or invalid AIN format",
	[AIN_INVALID]             = "Invalid AIN file"
};

const char *ain_strerror(int error)
{
	if (error < AIN_MAX_ERROR)
		return errtab[error];
	return "Invalid error code";
}

const char *ain_strtype(struct ain *ain, enum ain_data_type type, int struct_type)
{
	static char buf[1024] = { [1023] = '\0' };
	struct ain_type t = { .data = type, .struc = struct_type, .rank = 0 };
	char *str = ain_strtype_d(ain, &t);
	strncpy(buf, str, 1023);
	free(str);
	return buf;
}

static char *type_sprintf(const char *fmt, ...)
{
	va_list ap;
	char *buf = xmalloc(1024);
	va_start(ap, fmt);

	vsnprintf(buf, 1023, fmt, ap);
	buf[1023] = '\0';

	va_end(ap);

	return buf;
}

static char *array_type_string(const char *str, int rank)
{
	if (rank <= 1)
		return strdup(str);
	return type_sprintf("%s@%d", str, rank);
}

static char *container_type_string(struct ain *ain, struct ain_type *t)
{
	char *buf = xmalloc(1024);
	char *type = ain_strtype_d(ain, t->array_type);
	const char *container_type
		= t->data == AIN_ARRAY     ? "array"
		: t->data == AIN_REF_ARRAY ? "ref array"
		: t->data == AIN_WRAP      ? "wrap"
		: t->data == AIN_OPTION    ? "option"
		: "unknown_container";

	// XXX: need to add space for nested container types to avoid ambiguity
	//      with '>>' token
	switch (t->array_type ? t->array_type->data : AIN_VOID) {
	case AIN_ARRAY:
	case AIN_REF_ARRAY:
	case AIN_WRAP:
	case AIN_IFACE_WRAP:
	case AIN_OPTION:
		snprintf(buf, 1023, "%s<%s >", container_type, type);
		break;
	default:
		snprintf(buf, 1023, "%s<%s>", container_type, type);
	}

	free(type);
	buf[1023] = '\0';
	return buf;
}

char *ain_strtype_d(struct ain *ain, struct ain_type *v)
{
	char buf[1024];
	if (!v)
		return strdup("?");
	switch (v->data) {
	case AIN_VOID:                return strdup("void");
	case AIN_INT:                 return strdup("int");
	case AIN_FLOAT:               return strdup("float");
	case AIN_STRING:              return strdup("string");
	case AIN_STRUCT:
		if (v->struc == -1 || !ain)
			return strdup("hll_struct");
		return strdup(ain->structures[v->struc].name);
	case AIN_ARRAY_INT:           return array_type_string("array<int>", v->rank);
	case AIN_ARRAY_FLOAT:         return array_type_string("array<float>", v->rank);
	case AIN_ARRAY_STRING:        return array_type_string("array<string>", v->rank);
	case AIN_ARRAY_STRUCT:
		if (v->struc == -1 || !ain)
			return array_type_string("array<struct>", v->rank);
		snprintf(buf, 1024, "array<%s>", ain->structures[v->struc].name);
		return array_type_string(buf, v->rank);
	case AIN_REF_INT:             return strdup("ref int");
	case AIN_REF_FLOAT:           return strdup("ref float");
	case AIN_REF_STRING:          return strdup("ref string");
	case AIN_REF_STRUCT:
		if (v->struc == -1 || !ain)
			return strdup("ref hll_struct");
		return type_sprintf("ref %s", ain->structures[v->struc].name);
	case AIN_REF_ARRAY_INT:       return array_type_string("ref array<int>", v->rank);
	case AIN_REF_ARRAY_FLOAT:     return array_type_string("ref array<float>", v->rank);
	case AIN_REF_ARRAY_STRING:    return array_type_string("ref array<string>", v->rank);
	case AIN_REF_ARRAY_STRUCT:
		if (v->struc == -1 || !ain)
			return strdup("ref array<hll_struct>");
		snprintf(buf, 1024, "ref array<%s>", ain->structures[v->struc].name);
		return array_type_string(buf, v->rank);
	case AIN_IMAIN_SYSTEM:        return strdup("imain_system");
	case AIN_FUNC_TYPE:           return strdup("functype");
	case AIN_ARRAY_FUNC_TYPE:     return array_type_string("array<functype>", v->rank);
	case AIN_REF_FUNC_TYPE:       return strdup("ref functype");
	case AIN_REF_ARRAY_FUNC_TYPE: return array_type_string("ref array<functype>", v->rank);
	case AIN_BOOL:                return strdup("bool");
	case AIN_ARRAY_BOOL:          return array_type_string("array<bool>", v->rank);
	case AIN_REF_BOOL:            return strdup("ref bool");
	case AIN_REF_ARRAY_BOOL:      return array_type_string("ref array<bool>", v->rank);
	case AIN_LONG_INT:            return strdup("lint");
	case AIN_ARRAY_LONG_INT:      return array_type_string("array<lint>", v->rank);
	case AIN_REF_LONG_INT:        return strdup("ref lint");
	case AIN_REF_ARRAY_LONG_INT:  return array_type_string("ref array<lint>", v->rank);
	case AIN_DELEGATE:            return strdup("delegate");
	case AIN_ARRAY_DELEGATE:      return array_type_string("array<delegate>", v->rank);
	case AIN_REF_DELEGATE:        return strdup("ref delegate");
	case AIN_REF_ARRAY_DELEGATE:  return array_type_string("ref array<delegate>", v->rank);
	case AIN_HLL_PARAM:           return strdup("hll_param");
	case AIN_REF_HLL_PARAM:       return strdup("ref hll_param");
	case AIN_ARRAY:
	case AIN_REF_ARRAY:
	case AIN_WRAP:
	case AIN_OPTION:
		return container_type_string(ain, v);
	case AIN_UNKNOWN_TYPE_87:     return strdup("type_87");
	case AIN_IFACE:
		if (v->struc == -1 || !ain)
			return strdup("interface");
		return strdup(ain->structures[v->struc].name);
	case AIN_ENUM2:
	case AIN_ENUM:
		if (v->struc == -1 || !ain || v->struc >= ain->nr_enums)
			return v->data == AIN_ENUM2 ? strdup("enum#91") : strdup("enum#92");
		return type_sprintf("%s#%d", ain->enums[v->struc].name, v->data);
	case AIN_REF_ENUM:
		if (v->struc == -1 || !ain || v->struc >= ain->nr_enums)
			return strdup("ref enum");
		return type_sprintf("ref %s", ain->enums[v->struc].name);
	case AIN_HLL_FUNC_71:
		return strdup("hll_func_71");
	case AIN_HLL_FUNC:
		return strdup("hll_func");
	case AIN_IFACE_WRAP:
		// NOTE: this type is always wrapped in an iter, and always carries an interface struct type
		if (v->struc == -1 || !ain || v->struc >= ain->nr_structures)
			return strdup("iwrap<?>");
		return type_sprintf("iwrap<%s>", ain->structures[v->struc].name);
	default:
		WARNING("Unknown type: %d", v->data);
		return type_sprintf("unknown_type_%d", v->data);
	}
}

const char *ain_variable_to_string(struct ain *ain, struct ain_variable *v)
{
	static char buf[2048] = { [2047] = '\0' };
	char *type = ain_strtype_d(ain, &v->type);
	int i = snprintf(buf, 2047, "%s %s", type, v->name);
	free(type);

	if (v->has_initval) {
		switch (v->type.data) {
		case AIN_STRING: {
			// FIXME: string needs escaping
			snprintf(buf+i, 2047-i, " = \"%s\"", v->initval.s);
			break;
		}
		case AIN_DELEGATE:
		case AIN_REF_TYPE:
			break;
		case AIN_FLOAT:
			snprintf(buf+i, 2047-i, " = %f", v->initval.f);
			break;
		default:
			snprintf(buf+i, 2047-i, " = %d", v->initval.i);
			break;
		}
	}
	return buf;
}

struct ain_reader {
	uint8_t *buf;
	size_t size;
	size_t index;
	struct ain_section *section;
	struct ain *ain;
	char *(*conv)(const char*);
};

static int32_t read_int32(struct ain_reader *r)
{
	int32_t v = LittleEndian_getDW(r->buf, r->index);
	r->index += 4;
	return v;
}

static uint8_t *read_bytes(struct ain_reader *r, size_t len)
{
	uint8_t *bytes = xmalloc(len);
	memcpy(bytes, r->buf + r->index, len);
	r->index += len;
	return bytes;
}

static char *read_string(struct ain_reader *r)
{
	char *input_str = (char*)r->buf + r->index;
	char *str = r->conv(input_str);
	r->index += strlen(input_str) + 1;
	return str;
}

// TODO: should just memcpy the whole section and create pointers into it
//       e.g. char *_messages; // raw AIN section
//            char **messages; // array of pointers into _messages
static char **read_strings(struct ain_reader *r, int count)
{
	char **strings = calloc(count, sizeof(char*));
	for (int i = 0; i < count; i++) {
		strings[i] = read_string(r);
	}
	return strings;
}

static struct string *read_vm_string(struct ain_reader *r)
{
	char *cstr = read_string(r);
	struct string *s = cstr_to_string(cstr);
	s->cow = true;
	free(cstr);
	return s;
}

static struct string **read_vm_strings(struct ain_reader *r, int count)
{
	struct string **strings = calloc(count, sizeof(struct string*));
	for (int i = 0; i < count; i++) {
		strings[i] = read_vm_string(r);
	}
	return strings;
}

static struct string *read_msg1_string(struct ain_reader *r)
{
	int32_t len = read_int32(r);
	char *bytes = xmalloc(len+1);
	memcpy(bytes, r->buf + r->index, len);
	bytes[len] = '\0';
	r->index += len;

	// why...
	for (int i = 0; i < len; i++) {
		bytes[i] -= (uint8_t)i;
		bytes[i] -= 0x60;
	}

	char *str = r->conv(bytes);
	struct string *s = cstr_to_string(str);
	free(str);
	free(bytes);

	return s;
}

static struct string **read_msg1_strings(struct ain_reader *r, int count)
{
	struct string **strings = calloc(count, sizeof(struct string*));
	for (int i = 0; i < count; i++) {
		strings[i] = read_msg1_string(r);
	}
	return strings;
}

static void read_variable_type(struct ain_reader *r, struct ain_type *t);

static struct ain_type *read_array_type(struct ain_reader *r)
{
	struct ain_type *type = xcalloc(1, sizeof(struct ain_type));
	read_variable_type(r, type);
	return type;
}

static void read_variable_initval(struct ain_reader *r, struct ain_variable *v)
{
	if ((v->has_initval = read_int32(r))) {
		if (v->has_initval != 1)
			WARNING("variable->has_initval is not boolean: %d (at %p)", v->has_initval, r->index - 4);
		switch (v->type.data) {
		case AIN_STRING:
			v->initval.s = read_string(r);
			break;
		case AIN_STRUCT:
		case AIN_DELEGATE:
		case AIN_REF_TYPE:
		case AIN_ARRAY:
			break;
		default:
			v->initval.i = read_int32(r);
		}
	}
}

static void read_variable_type(struct ain_reader *r, struct ain_type *t)
{
	t->data  = read_int32(r);
	t->struc = read_int32(r);
	t->rank  = read_int32(r);

	// XXX: in v11+, 'rank' is a boolean which indicates the presence or
	//      absence of an additional sub-type (which may itself have a
	//      sub-type, etc.). Arrays no longer have ranks, but are instead
	//      nested. Also, the struct type of the most deeply nested type
	//      is propagated to all parent types for some reason (e.g. an
	//      array of struct#13 will have struct type 13).
	if (AIN_VERSION_GTE(r->ain, 11, 0)) {
		if (t->rank < 0 || t->rank > 1)
			WARNING("non-boolean rank in ain v11+ (%d, %d, %d)", t->data, t->struc, t->rank);
		if (t->rank)
			t->array_type = read_array_type(r);
	}
}

static struct ain_variable *read_variables(struct ain_reader *r, int count, struct ain *ain, enum ain_variable_type var_type)
{
	struct ain_variable *variables = calloc(count, sizeof(struct ain_variable));
	for (int i = 0; i < count; i++) {
		struct ain_variable *v = &variables[i];
		v->var_type = var_type;
		v->name = read_string(r);
		if (AIN_VERSION_GTE(ain, 12, 0))
			v->name2 = read_string(r); // ???
		read_variable_type(r, &v->type);
		if (AIN_VERSION_GTE(ain, 8, 0))
			read_variable_initval(r, v);
	}
	return variables;
}

static void read_return_type(struct ain_reader *r, struct ain_type *t, struct ain *ain)
{
	if (AIN_VERSION_GTE(ain, 11, 0)) {
		read_variable_type(r, t);
		return;
	}

	t->data  = read_int32(r);
	t->struc = read_int32(r);
}

static struct ain_function *read_functions(struct ain_reader *r, int count, struct ain *ain)
{
	struct ain_function *funs = calloc(count, sizeof(struct ain_function));
	for (int i = 0; i < count; i++) {
		funs[i].address = read_int32(r);
		// XXX: Fix for broken CN dohnadohna.ain
		//      Typically 0xFF is not valid as a first byte, so this is probably OK
		if (r->buf[r->index] == 0xFF) {
			WARNING("Junk at start of function name: '%s'", r->buf + r->index);
			while (r->buf[r->index] == 0xFF)
				r->index++;
		}
		funs[i].name = read_string(r);
		if (!strcmp(funs[i].name, "0"))
			ain->alloc = i;

		// detect game (to apply needed quirks)
		if (ain->version == 14 && ain->minor_version == 1) {
			// Evenicle 2
			if (!strcmp(funs[i].name, "C_MedicaMenu@0")) {
				ain->minor_version = 0;
			}
			// Haha Ranman
			else if (!strcmp(funs[i].name, "CInvasionHexScene@0")) {
				ain->minor_version = 0;
			}
			else if (!strcmp(funs[i].name, "_ALICETOOLS_AINV14_00")) {
				ain->minor_version = 0;
			}
		}

		if (ain->version > 1 && ain->version < 7)
			funs[i].is_label = read_int32(r);

		read_return_type(r, &funs[i].return_type, ain);

		funs[i].nr_args = read_int32(r);
		funs[i].nr_vars = read_int32(r);

		if (AIN_VERSION_GTE(ain, 11, 0)) {
			funs[i].is_lambda = read_int32(r); // known values: 0, 1
			if (funs[i].is_lambda && funs[i].is_lambda != 1) {
				WARNING("function->is_lambda is not a boolean: %d (at %p)", funs[i].is_lambda, r->index - 4);
			}
		}

		if (ain->version > 1) {
			funs[i].crc = read_int32(r);
		}
		if (funs[i].nr_vars > 0) {
			funs[i].vars = read_variables(r, funs[i].nr_vars, ain, AIN_VAR_LOCAL);
		}
	}
	return funs;
}

static struct ain_variable *read_globals(struct ain_reader *r, int count, struct ain *ain)
{
	struct ain_variable *globals = calloc(count, sizeof(struct ain_variable));
	for (int i = 0; i < count; i++) {
		globals[i].name = read_string(r);
		if (AIN_VERSION_GTE(ain, 12, 0))
			globals[i].name2 = read_string(r);
		read_variable_type(r, &globals[i].type);
		if (AIN_VERSION_GTE(ain, 5, 0))
			globals[i].group_index = read_int32(r);
		globals[i].var_type = AIN_VAR_GLOBAL;
	}
	return globals;
}

static struct ain_initval *read_initvals(struct ain_reader *r, int count)
{
	struct ain_initval *values = calloc(count, sizeof(struct ain_initval));
	for (int i = 0; i < count; i++) {
		values[i].global_index = read_int32(r);
		values[i].data_type = read_int32(r);
		if (values[i].data_type == AIN_STRING) {
			values[i].string_value = read_string(r);
		} else {
			values[i].int_value = read_int32(r);
		}
	}
	return values;
}

static struct ain_struct *read_structures(struct ain_reader *r, int count, struct ain *ain)
{
	struct ain_struct *structures = calloc(count, sizeof(struct ain_struct));
	for (int i = 0; i < count; i++) {
		structures[i].name = read_string(r);
		if (AIN_VERSION_GTE(ain, 11, 0)) {
			structures[i].nr_interfaces = read_int32(r);
			structures[i].interfaces = xcalloc(structures[i].nr_interfaces, sizeof(struct ain_interface));
			for (int j = 0; j < structures[i].nr_interfaces; j++) {
				structures[i].interfaces[j].struct_type = read_int32(r);
				structures[i].interfaces[j].vtable_offset = read_int32(r);
			}
		}
		structures[i].constructor = read_int32(r);
		structures[i].destructor = read_int32(r);
		structures[i].nr_members = read_int32(r);
		structures[i].members = read_variables(r, structures[i].nr_members, ain, AIN_VAR_MEMBER);

		// Staring with Hentai Labyrinth, there is a list of functions at the end.
		// I believe this is a listing of the functions in the vtable.
		if (AIN_VERSION_GTE(ain, 14, 1)) {
			structures[i].nr_vmethods = read_int32(r);
			structures[i].vmethods = xcalloc(structures[i].nr_vmethods, sizeof(int32_t));
			for (int j = 0; j < structures[i].nr_vmethods; j++) {
				structures[i].vmethods[j] = read_int32(r);
			}
		}
	}

	if (AIN_VERSION_GTE(ain, 11, 0)) {
		for (int i = 0; i < count; i++) {
			for (int j = 0; j < structures[i].nr_interfaces; j++) {
				int struct_type = structures[i].interfaces[j].struct_type;
				assert(struct_type >= 0 && struct_type < count);
				structures[struct_type].is_interface = true;
			}
		}
	}

	return structures;
}

static struct ain_hll_argument *read_hll_arguments(struct ain_reader *r, int count)
{
	struct ain_hll_argument *arguments = calloc(count, sizeof(struct ain_hll_argument));
	for (int i = 0; i < count; i++) {
		arguments[i].name = read_string(r);
		if (AIN_VERSION_GTE(r->ain, 14, 0)) {
			read_variable_type(r, &arguments[i].type);
		} else {
			arguments[i].type.data = read_int32(r);
			arguments[i].type.struc = -1;
			arguments[i].type.rank = 0;
		}
	}
	return arguments;
}

static struct ain_hll_function *read_hll_functions(struct ain_reader *r, int count)
{
	struct ain_hll_function *functions = calloc(count, sizeof(struct ain_hll_function));
	for (int i = 0; i < count; i++) {
		functions[i].name = read_string(r);
		if (AIN_VERSION_GTE(r->ain, 14, 0)) {
			read_variable_type(r, &functions[i].return_type);
		} else {
			functions[i].return_type.data = read_int32(r);
			functions[i].return_type.struc = -1;
			functions[i].return_type.rank = 0;
		}
		functions[i].nr_arguments = read_int32(r);
		if (functions[i].nr_arguments > 100 || functions[i].nr_arguments < 0)
			ERROR("TOO MANY ARGUMENTS (AT 0x%x)", r->index);
		functions[i].arguments = read_hll_arguments(r, functions[i].nr_arguments);
	}
	return functions;
}

static struct ain_library *read_libraries(struct ain_reader *r, int count)
{
	struct ain_library *libraries = calloc(count, sizeof(struct ain_library));
	for (int i = 0; i < count; i++) {
		libraries[i].name = read_string(r);
		libraries[i].nr_functions = read_int32(r);
		libraries[i].functions = read_hll_functions(r, libraries[i].nr_functions);
	}
	return libraries;
}

static struct ain_switch_case *read_switch_cases(struct ain_reader *r, int count, struct ain_switch *parent)
{
	struct ain_switch_case *cases = calloc(count, sizeof(struct ain_switch));
	for (int i = 0; i < count; i++) {
		cases[i].value = read_int32(r);
		cases[i].address = read_int32(r);
		cases[i].parent = parent;
	}
	return cases;
}

static struct ain_switch *read_switches(struct ain_reader *r, int count)
{
	struct ain_switch *switches = calloc(count, sizeof(struct ain_switch));
	for (int i = 0; i < count; i++) {
		switches[i].case_type = read_int32(r);
		switches[i].default_address = read_int32(r);
		switches[i].nr_cases = read_int32(r);
		switches[i].cases = read_switch_cases(r, switches[i].nr_cases, &switches[i]);
	}
	return switches;
}

static struct ain_scenario_label *read_scenario_labels(struct ain_reader *r, int count)
{
	struct ain_scenario_label *labels = xcalloc(count, sizeof(struct ain_scenario_label));
	for (int i = 0; i < count; i++) {
		labels[i].name = read_string(r);
		labels[i].address = read_int32(r);
	}
	return labels;
}

static struct ain_function_type *read_function_types(struct ain_reader *r, int count, struct ain *ain)
{
	struct ain_function_type *types = calloc(count, sizeof(struct ain_function_type));
	for (int i = 0; i < count; i++) {
		types[i].name = read_string(r);
		read_return_type(r, &types[i].return_type, ain);
		types[i].nr_arguments = read_int32(r);
		types[i].nr_variables = read_int32(r);
		types[i].variables = read_variables(r, types[i].nr_variables, ain, AIN_VAR_LOCAL);
	}
	return types;
}

#define for_each_instruction(ain, addr, instr, start, user_code)	\
	do {								\
		const struct instruction *instr;			\
		for (size_t addr = start; addr < ain->code_size; addr += instruction_width(instr->opcode)) { \
			uint16_t _fei_opcode = LittleEndian_getW(ain->code, addr); \
			if (_fei_opcode >= NR_OPCODES) {		\
				WARNING("Unknown/invalid opcode: %u", _fei_opcode); \
				break;					\
			}						\
			instr = &instructions[_fei_opcode];		\
			if (addr + instr->nr_args * 4 >= ain->code_size) { \
				WARNING("CODE section truncated?");	\
				break;					\
			}						\
			user_code;					\
		}							\
	} while (0)

struct ain_enum *read_enums(struct ain_reader *r, int count, struct ain *ain)
{
	char **names = read_strings(r, count);
	struct ain_enum *enums = xcalloc(count, sizeof(struct ain_enum));

	for (int i = 0; i < count; i++) {
		enums[i].name = names[i];

		char buf[1024] = { [1023] = '\0' };
		snprintf(buf, 1023, ain->version < 14 ? "%s@String" : "%s::ToString", names[i]);

		struct func_list *funs = get_function(ain, buf);
		if (!funs || funs->nr_slots != 1) {
			WARNING("Failed to parse enum: %s", names[i]);
			continue;
		}

		int j = 0;
		for_each_instruction(ain, addr, instr, ain->functions[funs->slots[0]].address, {
			if (instr->opcode == ENDFUNC)
				break;
			if (instr->opcode != S_PUSH)
				continue;

			int32_t strno = LittleEndian_getDW(ain->code, addr + 2);
			if (strno < 0 || strno >= ain->nr_strings) {
				WARNING("Encountered invalid string number when parsing enums");
				continue;
			}
			if (!ain->strings[strno]->size)
				continue;

			enums[i].symbols = xrealloc(enums[i].symbols, sizeof(char*) * (j+1));
			enums[i].symbols[j] = ain->strings[strno]->text;
			enums[i].nr_symbols = j + 1;
			j++;
		});
	}

	free(names);
	return enums;
}

static void start_section(struct ain_reader *r, struct ain_section *section)
{
	if (r->section)
		r->section->size = r->index - r->section->addr;
	r->section = section;
	if (section) {
		r->section->addr = r->index;
		r->section->present = true;
		r->index += 4;
	}
}

static bool read_tag(struct ain_reader *r, struct ain *ain)
{
	if (r->index + 4 >= r->size) {
		start_section(r, NULL);
		return false;
	}

	uint8_t *tag_loc = r->buf + r->index;

#define TAG_EQ(tag) !strncmp((char*)tag_loc, tag, 4)
	// FIXME: need to check len or could segfault on corrupt AIN file
	if (TAG_EQ("VERS")) {
		start_section(r, &ain->VERS);
		ain->version = read_int32(r);
		if (AIN_VERSION_GTE(ain, 11, 0)) {
			instructions[CALLHLL].nr_args = 3;
			instructions[NEW].nr_args = 2;
			instructions[S_MOD].nr_args = 1;
			instructions[OBJSWAP].nr_args = 1;
			instructions[DG_STR_TO_METHOD].nr_args = 1;
			instructions[CALLMETHOD].args[0] = T_INT;
		}
		// XXX: default to 14.1 (14.0 games handled individually)
		if (ain->version == 14)
			ain->minor_version = 1;
	} else if (TAG_EQ("KEYC")) {
		start_section(r, &ain->KEYC);
		ain->keycode = read_int32(r);
	} else if (TAG_EQ("CODE")) {
		start_section(r, &ain->CODE);
		ain->code_size = read_int32(r);
		ain->code = read_bytes(r, ain->code_size);
	} else if (TAG_EQ("FUNC")) {
		start_section(r, &ain->FUNC);
		ain->nr_functions = read_int32(r);
		ain->functions = read_functions(r, ain->nr_functions, ain);
		ain_index_functions(ain);
	} else if (TAG_EQ("GLOB")) {
		start_section(r, &ain->GLOB);
		ain->nr_globals = read_int32(r);
		ain->globals = read_globals(r, ain->nr_globals, ain);
	} else if (TAG_EQ("GSET")) {
		start_section(r, &ain->GSET);
		ain->nr_initvals = read_int32(r);
		ain->global_initvals = read_initvals(r, ain->nr_initvals);
	} else if (TAG_EQ("STRT")) {
		start_section(r, &ain->STRT);
		ain->nr_structures = read_int32(r);
		ain->structures = read_structures(r, ain->nr_structures, ain);
		ain_index_structures(ain);
	} else if (TAG_EQ("MSG0")) {
		start_section(r, &ain->MSG0);
		ain->nr_messages = read_int32(r);
		ain->messages = read_vm_strings(r, ain->nr_messages);
	} else if (TAG_EQ("MSG1")) {
		start_section(r, &ain->MSG1);
		ain->nr_messages = read_int32(r);
		ain->msg1_uk = read_int32(r); // ???
		ain->messages = read_msg1_strings(r, ain->nr_messages);
	} else if (TAG_EQ("MAIN")) {
		start_section(r, &ain->MAIN);
		ain->main = read_int32(r);
	} else if (TAG_EQ("MSGF")) {
		start_section(r, &ain->MSGF);
		ain->msgf = read_int32(r);
	} else if (TAG_EQ("HLL0")) {
		start_section(r, &ain->HLL0);
		ain->nr_libraries = read_int32(r);
		ain->libraries = read_libraries(r, ain->nr_libraries);
	} else if (TAG_EQ("SWI0")) {
		start_section(r, &ain->SWI0);
		ain->nr_switches = read_int32(r);
		ain->switches = read_switches(r, ain->nr_switches);
	} else if (TAG_EQ("GVER")) {
		start_section(r, &ain->GVER);
		ain->game_version = read_int32(r);
	} else if (TAG_EQ("SLBL")) {
		start_section(r, &ain->SLBL);
		ain->SLBL.present = true;
		ain->nr_scenario_labels = read_int32(r);
		ain->scenario_labels = read_scenario_labels(r, ain->nr_scenario_labels);
	} else if (TAG_EQ("STR0")) {
		start_section(r, &ain->STR0);
		ain->nr_strings = read_int32(r);
		ain->strings = read_vm_strings(r, ain->nr_strings);
	} else if (TAG_EQ("FNAM")) {
		start_section(r, &ain->FNAM);
		ain->nr_filenames = read_int32(r);
		ain->filenames = read_strings(r, ain->nr_filenames);
	} else if (TAG_EQ("OJMP")) {
		start_section(r, &ain->OJMP);
		ain->ojmp = read_int32(r);
	} else if (TAG_EQ("FNCT")) {
		start_section(r, &ain->FNCT);
		ain->fnct_size = read_int32(r);
		ain->nr_function_types = read_int32(r);
		ain->function_types = read_function_types(r, ain->nr_function_types, ain);
	} else if (TAG_EQ("DELG")) {
		start_section(r, &ain->DELG);
		ain->delg_size = read_int32(r);
		ain->nr_delegates = read_int32(r);
		ain->delegates = read_function_types(r, ain->nr_delegates, ain);
	} else if (TAG_EQ("OBJG")) {
		start_section(r, &ain->OBJG);
		ain->nr_global_groups = read_int32(r);
		ain->global_group_names = read_strings(r, ain->nr_global_groups);
	} else if (TAG_EQ("ENUM")) {
		start_section(r, &ain->ENUM);
		ain->ENUM.present = true;
		ain->nr_enums = read_int32(r);
		ain->enums = read_enums(r, ain->nr_enums, ain);
	} else {
		start_section(r, NULL);
		WARNING("Junk at end of AIN file?");
		return false;
	}
#undef TAG_EQ

	return true;
}

static void distribute_initvals(struct ain *ain)
{
	for (int i = 0; i < ain->nr_initvals; i++) {
		struct ain_variable *g = &ain->globals[ain->global_initvals[i].global_index];
		g->has_initval = true;
		if (ain->global_initvals[i].data_type == AIN_STRING)
			g->initval.s = ain->global_initvals[i].string_value;
		else
			g->initval.i = ain->global_initvals[i].int_value;
	}
}

static uint8_t *decompress_ain(uint8_t *in, long *len)
{
	uint8_t *out;
	long out_len = LittleEndian_getDW(in, 8);
	long in_len  = LittleEndian_getDW(in, 12);

	if (out_len < 0 || in_len < 0)
		return NULL;

	out = xmalloc(out_len);
	int r = uncompress(out, (unsigned long*)&out_len, in+16, in_len);
	if (r != Z_OK) {
		if (r == Z_BUF_ERROR)
			WARNING("uncompress failed: Z_BUF_ERROR");
		else if (r == Z_MEM_ERROR)
			WARNING("uncompress failed: Z_MEM_ERROR");
		else if (r == Z_DATA_ERROR)
			WARNING("uncompress failed: Z_DATA_ERROR");
		free(out);
		return NULL;
	}

	*len = out_len;
	return out;
}

void ain_decrypt(uint8_t *buf, size_t len)
{
	mt19937_xorcode(buf, len, 0x5D3E3);
}

static bool ain_is_encrypted(uint8_t *buf)
{
	uint8_t magic[8];

	memcpy(magic, buf, 8);
	ain_decrypt(magic, 8);

	return !strncmp((char*)magic, "VERS", 4) && !magic[5] && !magic[6] && !magic[7];
}

uint8_t *ain_read(const char *path, long *len, int *error)
{
	FILE *fp;
	uint8_t *buf = NULL;

	if (!(fp = file_open_utf8(path, "rb"))) {
		*error = AIN_FILE_ERROR;
		goto err;
	}

	// get size of AIN file
	fseek(fp, 0, SEEK_END);
	*len = ftell(fp);
	fseek(fp, 0, SEEK_SET);

	// read AIN file into memory
	buf = xmalloc(*len + 4); // why +4?
	if (fread(buf, *len, 1, fp) != 1) {
		*error = AIN_FILE_ERROR;
		fclose(fp);
		goto err;
	}
	fclose(fp);

	// check magic
	if (!strncmp((char*)buf, "AI2\0\0\0\0", 8)) {
		uint8_t *uc = decompress_ain(buf, len);
		if (!uc) {
			*error = AIN_INVALID;
			goto err;
		}
		free(buf);
		buf = uc;
	} else if (ain_is_encrypted(buf)) {
		ain_decrypt(buf, *len);
	} else {
		printf("%.4s\n", buf);
		*error = AIN_UNRECOGNIZED_FORMAT;
		goto err;
	}

	return buf;
err:
	free(buf);
	return NULL;
}

struct ain *ain_open_conv(const char *path, char*(*conv)(const char*), int *error)
{
	long len;
	struct ain *ain = NULL;
	uint8_t *buf = ain_read(path, &len, error);
	if (!buf)
		goto err;

	// read data into ain struct
	ain = xcalloc(1, sizeof(struct ain));
	ain->ain_path = xstrdup(path);
	struct ain_reader r = {
		.buf = buf,
		.index = 0,
		.size = len,
		.ain = ain,
		.conv = conv,
	};
	ain->version = -1;
	ain->alloc = -1;
	while (read_tag(&r, ain));
	if (ain->version == -1) {
		*error = AIN_INVALID;
		goto err;
	}
	if (ain->MSG1.present && ain->version == 6) {
		ain->minor_version = max(ain->minor_version, 1);
	}
	distribute_initvals(ain);

	free(buf);
	*error = AIN_SUCCESS;
	return ain;
err:
	free(buf);
	free(ain);
	return NULL;
}

struct ain *ain_open(const char *path, int *error)
{
	return ain_open_conv(path, strdup, error);
}

struct ain *ain_new(int major_version, int minor_version)
{
	struct ain *ain = xcalloc(1, sizeof(struct ain));
	ain->VERS.present = true;
	ain->KEYC.present = major_version < 12;
	ain->CODE.present = true;
	ain->FUNC.present = true;
	ain->GLOB.present = true;
	ain->GSET.present = major_version < 12;
	ain->STRT.present = true;
	// XXX: Starting with Rance IX, MSG1 section is used instead of MSG0.
	//      We can't tell from the ain version alone whether MSG0 or MSG1
	//      should be used since the ain version did not increase with
	//      this change, but v7+ at least should always use MSG1.
	ain->MSG1.present = (major_version == 6 && minor_version > 0) || major_version > 6;
	ain->MSG0.present = !ain->MSG1.present;
	ain->MAIN.present = true;
	ain->MSGF.present = major_version < 12;
	ain->HLL0.present = true;
	ain->SWI0.present = true;
	ain->GVER.present = true;
	ain->SLBL.present = major_version == 1;
	ain->STR0.present = true;
	ain->FNAM.present = major_version < 12;
	ain->OJMP.present = major_version < 7;
	// XXX: Another change starting from Rance IX: FNCT section disappears.
	//      Maybe they just stopped using function types, but still support
	//      them in the VM? Will need to change this flag when function
	//      types are added to the ain file.
	ain->FNCT.present = major_version < 7;
	// XXX: Starting with Oyako Rankan... earlier v6 games don't have DELG.
	//      Will need to change this flag when delegates are added to the
	//      ain file.
	ain->DELG.present = major_version >= 7;
	ain->OBJG.present = major_version >= 5;
	ain->ENUM.present = major_version >= 12;

	ain->version = major_version;
	ain->minor_version = minor_version;
	ain->main = -1;
	ain->msgf = -1;
	ain->ojmp = -1;
	ain->game_version = 100;

	ain->alloc = -1;

	ain->nr_functions = 1;
	ain->functions = xcalloc(2, sizeof(struct ain_function));
	ain->functions[0].name = strdup("NULL");
	ain->functions[0].return_type = (struct ain_type) {
		.data = AIN_VOID,
		.struc = -1,
		.rank = 0
	};
	// XXX: If a minor version is given, we create a fake function so that
	//      aindump can determine the minor version
	if (minor_version) {
		ain->nr_functions = 2;
		ain->functions[1].name = xmalloc(strlen("_ALICETOOLS_AINVXX_XX")+1);
		sprintf(ain->functions[1].name, "_ALICETOOLS_AINV%02d_%02d", major_version, minor_version);
		ain->functions[0].return_type = (struct ain_type) {
			.data = AIN_VOID,
			.struc = -1,
			.rank = 0
		};
	}

	ain->nr_messages = 1;
	ain->messages = xcalloc(1, sizeof(struct string*));
	ain->messages[0] = make_string("", 0);

	ain->nr_strings = 1;
	ain->strings = xcalloc(1, sizeof(struct string *));
	ain->strings[0] = make_string("", 0);

	ain_index_structures(ain);
	ain_index_functions(ain);

	return ain;
}

void ain_free_type(struct ain_type *type)
{
	if (type->array_type) {
		ain_free_type(type->array_type);
		free(type->array_type);
	}
}

void ain_free_variables(struct ain_variable *vars, int nr_vars)
{
	for (int i = 0; i < nr_vars; i++) {
		free(vars[i].name);
		free(vars[i].name2);
		ain_free_type(&vars[i].type);
		if (vars[i].has_initval && vars[i].type.data == AIN_STRING)
			free(vars[i].initval.s);
	}
	free(vars);
}

static void _ain_free_function_types(struct ain_function_type *funs, int n)
{
	for (int i = 0; i < n; i++) {
		free(funs[i].name);
		ain_free_type(&funs[i].return_type);
		ain_free_variables(funs[i].variables, funs[i].nr_variables);
	}
	free(funs);
}

static void ain_free_vmstrings(struct string **strings, int n)
{
	for (int i = 0; i < n; i++) {
		free_string(strings[i]);
	}
	free(strings);
}

static void ain_free_cstrings(char **strings, int n)
{
	for (int i = 0; i < n; i++) {
		free(strings[i]);
	}
	free(strings);
}

void ain_free_functions(struct ain *ain)
{
	for (int f = 0; f < ain->nr_functions; f++) {
		free(ain->functions[f].name);
		ain_free_type(&ain->functions[f].return_type);
		ain_free_variables(ain->functions[f].vars, ain->functions[f].nr_vars);
	}
	free(ain->functions);
	ain->functions = NULL;
	ain->nr_functions = 0;
}

void ain_free_globals(struct ain *ain)
{
	ain_free_variables(ain->globals, ain->nr_globals);
	ain->globals = NULL;
	ain->nr_globals = 0;
}

void ain_free_initvals(struct ain *ain)
{
	free(ain->global_initvals);
	ain->global_initvals = NULL;
	ain->nr_initvals = 0;
}

void ain_free_structures(struct ain *ain)
{
	for (int s = 0; s < ain->nr_structures; s++) {
		free(ain->structures[s].name);
		free(ain->structures[s].interfaces);
		free(ain->structures[s].vmethods);
		ain_free_variables(ain->structures[s].members, ain->structures[s].nr_members);
		_ain_free_function_types(ain->structures[s].iface_methods,
				ain->structures[s].nr_iface_methods);
	}
	free(ain->structures);
	ain->structures = NULL;
	ain->nr_structures = 0;
}

void ain_free_messages(struct ain *ain)
{
	ain_free_vmstrings(ain->messages, ain->nr_messages);
	ain->messages = NULL;
	ain->nr_messages = 0;
}

void ain_free_hll_argument(struct ain_hll_argument *arg)
{
	free(arg->name);
	ain_free_type(&arg->type);
}

void ain_free_hll_function(struct ain_hll_function *f)
{
	free(f->name);
	ain_free_type(&f->return_type);
	for (int i = 0; i < f->nr_arguments; i++) {
		ain_free_hll_argument(&f->arguments[i]);
	}
	free(f->arguments);
}

void ain_free_library(struct ain_library *lib)
{
	free(lib->name);
	for (int i = 0; i < lib->nr_functions; i++) {
		ain_free_hll_function(&lib->functions[i]);
	}
	free(lib->functions);
}

void ain_free_libraries(struct ain *ain)
{
	for (int i = 0; i < ain->nr_libraries; i++) {
		ain_free_library(&ain->libraries[i]);
	}
	free(ain->libraries);
	ain->libraries = NULL;
	ain->nr_libraries = 0;
}

void ain_free_switches(struct ain *ain)
{
	for (int i = 0; i < ain->nr_switches; i++) {
		free(ain->switches[i].cases);
	}
	free(ain->switches);
	ain->switches = NULL;
	ain->nr_switches = 0;
}

void ain_free_scenario_labels(struct ain *ain)
{
	for (int i = 0; i < ain->nr_scenario_labels; i++) {
		free(ain->scenario_labels[i].name);
	}
	free(ain->scenario_labels);
	ain->scenario_labels = NULL;
	ain->nr_scenario_labels = 0;
}

void ain_free_strings(struct ain *ain)
{
	ain_free_vmstrings(ain->strings, ain->nr_strings);
	ain->strings = NULL;
	ain->nr_strings = 0;
}

void ain_free_filenames(struct ain *ain)
{
	ain_free_cstrings(ain->filenames, ain->nr_filenames);
	ain->filenames = NULL;
	ain->nr_filenames = 0;
}

void ain_free_function_types(struct ain *ain)
{
	_ain_free_function_types(ain->function_types, ain->nr_function_types);
	ain->function_types = NULL;
	ain->nr_function_types = 0;
}

void ain_free_delegates(struct ain *ain)
{
	_ain_free_function_types(ain->delegates, ain->nr_delegates);
	ain->delegates = NULL;
	ain->nr_delegates = 0;
}

void ain_free_global_groups(struct ain *ain)
{
	ain_free_cstrings(ain->global_group_names, ain->nr_global_groups);
	ain->global_group_names = NULL;
	ain->nr_global_groups = 0;
}

void ain_free_enums(struct ain *ain)
{
	for (int i = 0; i < ain->nr_enums; i++) {
		free(ain->enums[i].name);
		free(ain->enums[i].symbols);
	}
	free(ain->enums);
	ain->enums = NULL;
	ain->nr_enums = 0;
}

void ain_free(struct ain *ain)
{
	free(ain->ain_path);
	free(ain->code);

	ht_foreach_value(ain->_func_ht, free);
	ht_free(ain->_func_ht);
	ht_free(ain->_struct_ht);
	if (ain->_string_ht)
		ht_free(ain->_string_ht);

	ain_free_functions(ain);
	ain_free_globals(ain);
	ain_free_initvals(ain);
	ain_free_structures(ain);
	ain_free_messages(ain);
	ain_free_libraries(ain);
	ain_free_switches(ain);
	ain_free_scenario_labels(ain);
	ain_free_strings(ain);
	ain_free_filenames(ain);
	ain_free_function_types(ain);
	ain_free_delegates(ain);
	ain_free_global_groups(ain);
	ain_free_enums(ain);

	free(ain);
}
