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

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <zlib.h>
#include "little_endian.h"
#include "system4.h"
#include "system4/afa.h"
#include "system4/archive.h"
#include "system4/buffer.h"
#include "system4/file.h"
#include "system4/hashtable.h"
#include "system4/string.h"

typedef struct string *(*string_conv_fun)(const char*,size_t);

static bool afa_exists(struct archive *ar, int no);
static bool afa_exists_by_name(struct archive *ar, const char *name, int *id_out);
static bool afa_exists_by_basename(struct archive *ar, const char *name, int *id_out);
static struct archive_data *afa_get(struct archive *ar, int no);
static struct archive_data *afa_get_by_name(struct archive *ar, const char *name);
static struct archive_data *afa_get_by_basename(struct archive *ar, const char *name);
static bool afa_load_file(struct archive_data *data);
static void afa_for_each(struct archive *ar, void (*iter)(struct archive_data *data, void *user), void *user);
static void afa_free_data(struct archive_data *data);
static void afa_free(struct archive *ar);

struct archive_ops afa_archive_ops = {
	.exists = afa_exists,
	.exists_by_name = afa_exists_by_name,
	.exists_by_basename = afa_exists_by_basename,
	.get = afa_get,
	.get_by_name = afa_get_by_name,
	.get_by_basename = afa_get_by_basename,
	.load_file = afa_load_file,
	.release_file = NULL,
	.copy_descriptor = NULL,
	.for_each = afa_for_each,
	.free_data = afa_free_data,
	.free = afa_free,
};

static struct afa_entry *afa_get_entry_by_name(struct afa_archive *ar, const char *name)
{
	if (!ar->name_index) {
		ar->name_index = ht_create(ar->nr_files * 3 / 2);
		for (unsigned i = 0; i < ar->nr_files; i++) {
			ht_put(ar->name_index, ar->files[i].name->text, &ar->files[i]);
		}
	}
	return ht_get(ar->name_index, name, NULL);
}

static struct afa_entry *afa_get_entry_by_basename(struct afa_archive *ar, const char *name)
{
	if (!ar->basename_index) {
		ar->basename_index = ht_create(ar->nr_files * 3 / 2);
		for (unsigned i = 0; i < ar->nr_files; i++) {
			char *basename = archive_basename(ar->files[i].name->text);
			ht_put(ar->basename_index, basename, &ar->files[i]);
			free(basename);
		}
	}

	char *basename = archive_basename(name);
	struct afa_entry *entry = ht_get(ar->basename_index, basename, NULL);
	free(basename);
	return entry;
}

static struct afa_entry *afa_get_entry_by_number(struct afa_archive *ar, int no)
{
	if (!ar->has_number) {
		return ((uint32_t)no < ar->nr_files) ? &ar->files[no] : NULL;
	}

	if (!ar->number_index) {
		ar->number_index = ht_create(ar->nr_files * 3 / 2);
		for (unsigned i = 0; i < ar->nr_files; i++) {
			ht_put_int(ar->number_index, ar->files[i].no, &ar->files[i]);
		}
	}
	return ht_get_int(ar->number_index, no, NULL);
}

static bool afa_exists(struct archive *_ar, int no)
{
	struct afa_archive *ar = (struct afa_archive*)_ar;
	return !!afa_get_entry_by_number(ar, no);
}

static bool afa_exists_by_name(struct archive *_ar, const char *name, int *id_out)
{
	struct afa_archive *ar = (struct afa_archive*)_ar;
	struct afa_entry *e = afa_get_entry_by_name(ar, name);
	if (!e)
		return false;
	if (id_out)
		*id_out = e->no;
	return true;
}

static bool afa_exists_by_basename(struct archive *_ar, const char *name, int *id_out)
{
	struct afa_archive *ar = (struct afa_archive*)_ar;
	struct afa_entry *e = afa_get_entry_by_basename(ar, name);
	if (!e)
		return false;
	if (id_out)
		*id_out = e->no;
	return true;
}

static bool afa_load_file(struct archive_data *data)
{
	if (data->data)
		return true;

	struct afa_archive *ar = (struct afa_archive*)data->archive;
	struct afa_entry *e = afa_get_entry_by_number(ar, data->no);

	if (ar->ar.mmapped) {
		data->data = (uint8_t*)ar->mmap_ptr + ar->data_start + e->off;
		return true;
	}

	fseek(ar->f, ar->data_start + e->off, SEEK_SET);

	data->data = xmalloc(e->size);
	if (fread(data->data, e->size, 1, ar->f) != 1) {
		WARNING("Failed to read '%s': %s", ar->filename, strerror(errno));
		free(data->data);
		free(data);
		return false;
	}

	return true;
}

struct archive_data *afa_entry_to_descriptor(struct afa_archive *ar, struct afa_entry *e)
{
	struct archive_data *data = xcalloc(1, sizeof(struct archive_data));
	data->size = e->size;
	data->name = strdup(e->name->text);
	data->no = e->no;
	data->archive = &ar->ar;
	return data;
}

static struct archive_data *afa_get_by_entry(struct afa_archive *ar, struct afa_entry *entry)
{
	if (!entry)
		return NULL;
	struct archive_data *data = afa_entry_to_descriptor(ar, entry);
	if (!afa_load_file(data)) {
		afa_free_data(data);
		return NULL;
	}
	return data;
}

static struct archive_data *afa_get(struct archive *_ar, int no)
{
	struct afa_archive *ar = (struct afa_archive*)_ar;
	return afa_get_by_entry(ar, afa_get_entry_by_number(ar, no));
}

static struct archive_data *afa_get_by_name(struct archive *_ar, const char *name)
{
	struct afa_archive *ar = (struct afa_archive*)_ar;
	return afa_get_by_entry(ar, afa_get_entry_by_name(ar, name));
}

static struct archive_data *afa_get_by_basename(struct archive *_ar, const char *name)
{
	struct afa_archive *ar = (struct afa_archive*)_ar;
	return afa_get_by_entry(ar, afa_get_entry_by_basename(ar, name));
}

static void afa_for_each(struct archive *_ar, void (*iter)(struct archive_data *data, void *user), void *user)
{
	struct afa_archive *ar = (struct afa_archive*)_ar;
	for (uint32_t i = 0; i < ar->nr_files; i++) {
		struct archive_data *data = afa_entry_to_descriptor(ar, &ar->files[i]);
		iter(data, user);
		afa_free_data(data);
	}
}

static void afa_free_data(struct archive_data *data)
{
	if (data->data && !data->archive->mmapped)
		free(data->data);
	free(data->name);
	free(data);
}

static void afa_free(struct archive *_ar)
{
	struct afa_archive *ar = (struct afa_archive*)_ar;
	for (uint32_t i = 0; i < ar->nr_files; i++) {
		free_string(ar->files[i].name);
	}
	if (ar->f)
		fclose(ar->f);
	if (ar->name_index)
		ht_free(ar->name_index);
	if (ar->basename_index)
		ht_free(ar->basename_index);
	if (ar->number_index)
		ht_free_int(ar->number_index);
	free(ar->filename);
	free(ar->files);
	free(ar);
}

static bool afa_read_entry(struct buffer *in, struct afa_archive *ar, struct afa_entry *entry,
			   int *error, string_conv_fun conv)
{
	uint32_t name_len = buffer_read_int32(in);
	entry->name = buffer_conv_pascal_string(in, conv); // NOTE: length is padded
	if (!entry->name) {
		*error = ARCHIVE_BAD_ARCHIVE_ERROR;
		return false;
	}
	entry->name->size = name_len; // fix length

	if (ar->has_number) {
		// XXX: Oyako Rankan is AFAv1 but all IDs are 0, which breaks load_file.
		//      We revert to using sequential indices in this case
		int32_t no = buffer_read_int32(in) - 1;
		if (no >= 0)
			entry->no = no;
	}
	entry->unknown0 = buffer_read_int32(in);
	entry->unknown1 = buffer_read_int32(in);

	entry->off = buffer_read_int32(in);
	entry->size = buffer_read_int32(in);
	return true;
}

static bool afa_determine_has_number(struct afa_archive *ar, struct buffer *in)
{
	if (ar->version == 1)
		return true;
	// The presence or absence of the ID field cannot be determined from the
	// file header, so we need to scan the file table.
	int nr_files = ar->nr_files;
	while (nr_files--) {
		if (buffer_remaining(in) < 8)
			return false;
		buffer_skip(in, 4);
		uint32_t name_len = buffer_read_int32(in);
		if (buffer_remaining(in) < name_len + 20)
			return false;
		buffer_skip(in, name_len + 20);
	}
	return buffer_remaining(in) == 0;
}

static bool afa_read_file_table(FILE *f, struct afa_archive *ar, int *error, string_conv_fun conv)
{
	uint8_t *buf = xmalloc(ar->compressed_size);
	uint8_t *table = xmalloc(ar->uncompressed_size);
	fseek(f, 44, SEEK_SET);
	if (fread(buf, ar->compressed_size, 1, f) != 1) {
		*error = ARCHIVE_FILE_ERROR;
		goto exit_err;
	}

	unsigned long uncompressed_size = ar->uncompressed_size;
	if (uncompress(table, &uncompressed_size, buf, ar->compressed_size) != Z_OK) {
		*error = ARCHIVE_BAD_ARCHIVE_ERROR;
		goto exit_err;
	}

	struct buffer r;
	buffer_init(&r, table, ar->uncompressed_size);
	ar->has_number = afa_determine_has_number(ar, &r);
	buffer_seek(&r, 0);
	ar->files = xcalloc(ar->nr_files, sizeof(struct afa_entry));
	for (uint32_t i = 0; i < ar->nr_files; i++) {
		ar->files[i].no = i;
		if (!afa_read_entry(&r, ar, &ar->files[i], error, conv)) {
			free(ar->files);
			goto exit_err;
		}
	}

	free(buf);
	free(table);
	return true;
exit_err:
	free(buf);
	free(table);
	return false;
}

bool afa3_read_metadata(char *hdr, FILE *f, struct afa_archive *ar, int *error, string_conv_fun conv);

static bool afa_read_metadata(FILE *f, struct afa_archive *ar, int *error, string_conv_fun conv)
{
	char buf[44];
	if (fread(buf, 44, 1, f) != 1) {
		*error = ARCHIVE_FILE_ERROR;
		return false;
	}

	fseek(f, 0, SEEK_END);
	ar->file_size = ftell(f);
	fseek(f, 0, SEEK_SET);

	if (strncmp(buf, "AFAH", 4)) {
		*error = ARCHIVE_BAD_ARCHIVE_ERROR;
		return false;
	}

	if (strncmp(buf+8, "AlicArch", 8)) {
		if (LittleEndian_getDW((uint8_t*)buf, 8) == 3) {
			return afa3_read_metadata(buf, f, ar, error, conv);
		}
		*error = ARCHIVE_BAD_ARCHIVE_ERROR;
		return false;
	}

	if (strncmp(buf+28, "INFO", 4) ||
	    LittleEndian_getDW((uint8_t*)buf, 4) != 0x1c) {
		*error = ARCHIVE_BAD_ARCHIVE_ERROR;
		return false;
	}

	ar->version = LittleEndian_getDW((uint8_t*)buf, 16);
	ar->unknown = LittleEndian_getDW((uint8_t*)buf, 20);
	ar->data_start = LittleEndian_getDW((uint8_t*)buf, 24);
	ar->compressed_size = LittleEndian_getDW((uint8_t*)buf, 32) - 16;
	ar->uncompressed_size = LittleEndian_getDW((uint8_t*)buf, 36);
	ar->nr_files = LittleEndian_getDW((uint8_t*)buf, 40);

	if (ar->data_start+8 >= ar->file_size) {
		*error = ARCHIVE_FILE_ERROR;
		return false;
	}

	fseek(f, ar->data_start, SEEK_SET);
	if (fread(buf, 8, 1, f) != 1) {
		*error = ARCHIVE_FILE_ERROR;
		return false;
	}
	fseek(f, 0, SEEK_SET);

	if (strncmp(buf, "DATA", 4)) {
		*error = ARCHIVE_BAD_ARCHIVE_ERROR;
		return false;
	}

	ar->data_size = LittleEndian_getDW((uint8_t*)buf, 4);
	if (ar->data_start + ar->data_size > ar->file_size) {
		*error = ARCHIVE_BAD_ARCHIVE_ERROR;
		return false;
	}

	return afa_read_file_table(f, ar, error, conv);
}

struct afa_archive *afa_open_conv(const char *file, int flags, int *error,
				  struct string *(*conv)(const char*,size_t))
{
#ifdef _WIN32
	flags &= ~ARCHIVE_MMAP;
#endif
	FILE *fp;
	struct afa_archive *ar = xcalloc(1, sizeof(struct afa_archive));

	if (!(fp = file_open_utf8(file, "rb"))) {
		WARNING("fopen failed: %s", strerror(errno));
		*error = ARCHIVE_FILE_ERROR;
		goto exit_err;
	}
	if (!afa_read_metadata(fp, ar, error, conv)) {
		WARNING("afa_read_metadata failed");
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
	ar->ar.ops = &afa_archive_ops;
	ar->ar.conv = conv;
	return ar;
exit_err:
	free(ar);
	return NULL;
}

struct afa_archive *afa_open(const char *file, int flags, int *error)
{
	return afa_open_conv(file, flags, error, make_string);
}
