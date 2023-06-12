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

#include <stdlib.h>
#include <string.h>
#include "system4.h"
#include "system4/ald.h"
#include "system4/utfsjis.h"

static const char *errtab[ARCHIVE_MAX_ERROR] = {
	[ARCHIVE_SUCCESS]           = "Success",
	[ARCHIVE_FILE_ERROR]        = "Error opening archive",
	[ARCHIVE_BAD_ARCHIVE_ERROR] = "Invalid archive"
};

/* Get a message describing an error. */
const char *archive_strerror(int error)
{
	if (error < ARCHIVE_MAX_ERROR)
		return errtab[error];
	return "Invalid error number";
}

void _archive_release_file(struct archive_data *data)
{
	if (!data->archive->mmapped)
		free(data->data);
	data->data = NULL;
}

/*
 * Default implementation for `archive_copy_descriptor`.
 * If the archive implementation extends the `archive_data` structure,
 * this must be overridden.
 */
void _archive_copy_descriptor_ip(struct archive_data *dst, struct archive_data *src)
{
	*dst = *src;
	dst->data = NULL;
	dst->name = strdup(dst->name);
}


struct archive_data *_archive_copy_descriptor(struct archive_data *src)
{
	struct archive_data *dst = xmalloc(sizeof(struct archive_data));
	_archive_copy_descriptor_ip(dst, src);
	return dst;
}

// FIXME?: assumes ASCII-compatible encoding
char *archive_basename(const char *name)
{
	char *basename = xstrdup(name);
	char *dot = strrchr(basename, '.');
	if (dot)
		*dot = '\0';
	sjis_normalize_path(basename);
	return basename;
}
