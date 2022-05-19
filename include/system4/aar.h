/* Copyright (C) 2022 kichikuou <KichikuouChrome@gmail.com>
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

#ifndef SYSTEM4_AAR_H
#define SYSTEM4_AAR_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include "system4/archive.h"

enum aar_entry_type {
	AAR_COMPRESSED = 0,
	AAR_RAW = 1,
	AAR_SYMLINK = -1,  // AAR v2+
};

struct aar_entry {
	uint32_t off;
	uint32_t size;
	enum aar_entry_type type;
	char *name;         // points inside aar_archive.index_buf
	char *link_target;  // points inside aar_archive.index_buf
};

struct aar_archive {
	struct archive ar;
	char *filename;
	size_t file_size;
	uint32_t version;
	uint32_t nr_files;
	struct aar_entry *files;
	uint8_t *index_buf;
	struct hash_table *ht;  // name -> aar_entry
	void *mmap_ptr;
	FILE *f;
};

struct aar_archive *aar_open(const char *file, int flags, int *error);

#endif /* SYSTEM4_AAR_H */
