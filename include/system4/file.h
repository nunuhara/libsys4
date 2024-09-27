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

#ifndef SYSTEM4_FILE_H
#define SYSTEM4_FILE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>
#include <dirent.h>

#ifdef _WIN32
typedef _WDIR UDIR;
typedef struct _stat64 ustat;
#else
typedef DIR UDIR;
typedef struct stat ustat;
#endif

struct stat;

UDIR *opendir_utf8(const char *path);
int closedir_utf8(UDIR *dir);
char *readdir_utf8(UDIR *dir);
int stat_utf8(const char *path, ustat *st);
char *realpath_utf8(const char *upath);
int remove_utf8(const char *path);
int rmdir_utf8(const char *path);
FILE *file_open_utf8(const char *path, const char *mode);
void *file_read(const char *path, size_t *len_out);
bool file_write(const char *path, uint8_t *data, size_t data_size);
bool file_copy(const char *src, const char *dst);
bool file_exists(const char *path);
off_t file_size(const char *path);
const char *file_extension(const char *path);
bool is_directory(const char *path);
int mkdir_p(const char *path);

char *path_dirname(const char *path);
char *path_basename(const char *path);
char *path_join(const char *dir, const char *base);
char *path_get_icase(const char *path);

#endif /* SYSTEM4_FILE_H */
