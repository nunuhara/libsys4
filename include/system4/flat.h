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

#ifndef SYSTEM4_FLAT_H
#define SYSTEM4_FLAT_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "system4/archive.h"

enum flat_data_type {
	FLAT_CG  = 2,
	FLAT_ZLIB = 5,
};

struct libl_entry {
	uint32_t unknown_size;
	uint32_t unknown_off;
	enum flat_data_type type;
	uint32_t size;
	uint32_t off;
};

struct talt_metadata {
	uint32_t unknown1_size;
	uint32_t unknown1_off;
	uint32_t unknown2;
	uint32_t unknown3;
	uint32_t unknown4;
	uint32_t unknown5;
};

struct talt_entry {
	uint32_t size;
	uint32_t off;
	uint32_t nr_meta;
	struct talt_metadata *metadata;
};

struct flat_data {
	struct archive_data super;
	size_t off;
	size_t size;
	enum flat_data_type type;
	bool allocated;
};

struct flat_section {
	bool present;
	size_t off;
	size_t size;
};

struct flat_archive {
	struct archive ar;

	// file map
	struct flat_section elna;
	struct flat_section flat;
	struct flat_section tmnl;
	struct flat_section mtlc;
	struct flat_section libl;
	struct flat_section talt;

	uint32_t nr_libl_entries;
	struct libl_entry *libl_entries;
	uint32_t nr_talt_entries;
	struct talt_entry *talt_entries;

	uint8_t *data;
};

struct flat_archive *flat_open(uint8_t *data, size_t size, int *error);
struct flat_archive *flat_open_file(const char *path, int flags, int *error);

#endif /* SYSTEM4_FLAT_H */
