/* Copyright (C) 2021 kichikuou <KichikuouChrome@gmail.com>
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

#ifndef SYSTEM4_ALK_H
#define SYSTEM4_ALK_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include "system4/archive.h"

struct alk_entry {
	uint32_t off;
	uint32_t size;
};

struct alk_archive {
	struct archive ar;
	char *filename;
	size_t file_size;
	struct alk_entry *files;
	int nr_files;
	void *mmap_ptr;
	FILE *f;
};

struct alk_archive *alk_open(const char *file, int flags, int *error);

#endif /* SYSTEM4_ALK_H */
