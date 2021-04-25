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

#ifndef SYSTEM4_DLF_H
#define SYSTEM4_DLF_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include "system4/archive.h"

#define DLF_NR_ENTRIES 300  // (dgn, dtx, tes) * 100

struct dlf_entry {
	uint32_t off;
	uint32_t size;
};

struct dlf_archive {
	struct archive ar;
	char *filename;
	size_t file_size;
	struct dlf_entry files[DLF_NR_ENTRIES];
	void *mmap_ptr;
	FILE *f;
};

struct dlf_archive *dlf_open(const char *file, int flags, int *error);

#endif /* SYSTEM4_DLF_H */
