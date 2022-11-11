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

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include "little_endian.h"
#include "system4.h"
#include "system4/archive.h"
#include "system4/alk.h"
#include "system4/file.h"

static bool alk_exists(struct archive *ar, int no);
static struct archive_data *alk_get(struct archive *ar, int no);
static bool alk_load_file(struct archive_data *data);
static void alk_for_each(struct archive *ar, void (*iter)(struct archive_data *data, void *user), void *user);
static void alk_free_data(struct archive_data *data);
static void alk_free(struct archive *_ar);

struct archive_ops alk_archive_ops = {
	.exists = alk_exists,
	.get = alk_get,
	.get_by_name = NULL,
	.get_by_basename = NULL,
	.load_file = alk_load_file,
	.release_file = NULL,
	.copy_descriptor = NULL,
	.for_each = alk_for_each,
	.free_data = alk_free_data,
	.free = alk_free,
};

static bool alk_exists(struct archive *_ar, int no)
{
	struct alk_archive *ar = (struct alk_archive*)_ar;
	if (no < 0 || no >= ar->nr_files)
		return false;
	return ar->files[no].size > 0;
}

static bool alk_load_file(struct archive_data *data)
{
	if (data->data)
		return true;

	struct alk_archive *ar = (struct alk_archive*)data->archive;
	struct alk_entry *e = &ar->files[data->no];

	if (ar->ar.mmapped) {
		data->data = (uint8_t*)ar->mmap_ptr + e->off;
		return true;
	}

	fseek(ar->f, e->off, SEEK_SET);

	data->data = xmalloc(e->size);
	if (fread(data->data, e->size, 1, ar->f) != 1) {
		WARNING("Failed to read '%s': %s", ar->filename, strerror(errno));
		free(data->data);
		data->data = NULL;
		return false;
	}

	return true;
}

static struct archive_data *alk_get_descriptor(struct archive *_ar, int no)
{
	struct alk_archive *ar = (struct alk_archive*)_ar;
	if (no < 0 || no >= ar->nr_files)
		return NULL;
	struct alk_entry *e = &ar->files[no];
	if (!e->size)
		return NULL;
	struct archive_data *data = xcalloc(1, sizeof(struct archive_data));
	data->size = e->size;
	data->name = xmalloc(16);
	sprintf(data->name, "%d", no);  // TODO: check file magic and add extension
	data->no = no;
	data->archive = _ar;
	return data;
}

static struct archive_data *alk_get(struct archive *_ar, int no)
{
	struct archive_data *data = alk_get_descriptor(_ar, no);
	if (!data)
		return NULL;
	if (!alk_load_file(data)) {
		alk_free_data(data);
		return NULL;
	}
	return data;
}

static void alk_for_each(struct archive *_ar, void (*iter)(struct archive_data *data, void *user), void *user)
{
	struct alk_archive *ar = (struct alk_archive*)_ar;
	for (uint32_t i = 0; i < ar->nr_files; i++) {
		struct archive_data *data = alk_get_descriptor(_ar, i);
		if (!data)
			continue;
		iter(data, user);
		alk_free_data(data);
	}
}

static void alk_free_data(struct archive_data *data)
{
	if (data->data && !data->archive->mmapped)
		free(data->data);
	free(data->name);
	free(data);
}

static void alk_free(struct archive *_ar)
{
	struct alk_archive *ar = (struct alk_archive*)_ar;
	if (ar->mmap_ptr)
		munmap(ar->mmap_ptr, ar->file_size);
	if (ar->f)
		fclose(ar->f);
	free(ar->files);
	free(ar->filename);
	free(ar);
}

static bool alk_read_header(FILE *f, struct alk_archive *ar, int *error)
{
	uint8_t buf[8];
	if (fread(buf, 8, 1, f) != 1) {
		*error = ARCHIVE_FILE_ERROR;
		return false;
	}

	if (memcmp(buf, "ALK0", 4)) {
		*error = ARCHIVE_BAD_ARCHIVE_ERROR;
		return false;
	}
	ar->nr_files = LittleEndian_getDW(buf, 4);
	ar->files = xcalloc(ar->nr_files, sizeof(struct alk_entry));
	for (int i = 0; i < ar->nr_files; i++) {
		if (fread(buf, 8, 1, f) != 1) {
			*error = ARCHIVE_FILE_ERROR;
			return false;
		}
		ar->files[i].off = LittleEndian_getDW(buf, 0);
		ar->files[i].size = LittleEndian_getDW(buf, 4);
	}

	fseek(f, 0, SEEK_END);
	ar->file_size = ftell(f);
	return true;
}

struct alk_archive *alk_open(const char *file, int flags, int *error)
{
#ifdef _WIN32
	flags &= ~ARCHIVE_MMAP;
#endif
	FILE *fp;
	struct alk_archive *ar = xcalloc(1, sizeof(struct alk_archive));

	if (!(fp = file_open_utf8(file, "rb"))) {
		WARNING("fopen failed: %s", strerror(errno));
		*error = ARCHIVE_FILE_ERROR;
		goto exit_err;
	}
	if (!alk_read_header(fp, ar, error)) {
		WARNING("alk_read_header failed");
		fclose(fp);
		goto exit_err;
	}
	if (flags & ARCHIVE_MMAP) {
		if (fclose(fp)) {
			WARNING("fclose failed: %s", strerror(errno));
			*error = ARCHIVE_FILE_ERROR;
			goto exit_err;
		}

		int fd = open(file, O_RDONLY);
		if (fd < 0) {
			WARNING("open failed: %s", strerror(errno));
			*error = ARCHIVE_FILE_ERROR;
			goto exit_err;
		}
		ar->mmap_ptr = mmap(0, ar->file_size, PROT_READ, MAP_SHARED, fd, 0);
		close(fd);
		if (ar->mmap_ptr == MAP_FAILED) {
			WARNING("mmap failed: %s", strerror(errno));
			*error = ARCHIVE_FILE_ERROR;
			goto exit_err;
		}
		ar->ar.mmapped = true;
	} else {
		ar->f = fp;
	}
	ar->filename = strdup(file);
	ar->ar.ops = &alk_archive_ops;
	return ar;
exit_err:
	free(ar);
	return NULL;
}
