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
#include "system4/aar.h"
#include "system4/archive.h"
#include "system4/file.h"
#include "system4/hashtable.h"
#include "system4/utfsjis.h"

static void *ht_get_ignorecase(struct hash_table *ht, const char *key, void *dflt)
{
	char *uc_key = strdup(key);
	sjis_normalize_path(uc_key);
	void *r = ht_get(ht, uc_key, dflt);
	free(uc_key);
	return r;
}

struct ht_slot *ht_put_ignorecase(struct hash_table *ht, const char *key, void *dflt)
{
	char *uc_key = strdup(key);
	sjis_normalize_path(uc_key);
	void *r = ht_put(ht, uc_key, dflt);
	free(uc_key);
	return r;
}

static bool aar_exists(struct archive *ar, int no);
static struct archive_data *aar_get(struct archive *ar, int no);
static struct archive_data *aar_get_by_name(struct archive *ar, const char *name);
static bool aar_load_file(struct archive_data *data);
static void aar_release_file(struct archive_data *data);
static void aar_for_each(struct archive *ar, void (*iter)(struct archive_data *data, void *user), void *user);
static void aar_free_data(struct archive_data *data);
static void aar_free(struct archive *ar);

struct archive_ops aar_archive_ops = {
	.exists = aar_exists,
	.get = aar_get,
	.get_by_name = aar_get_by_name,
	.get_by_basename = NULL,
	.load_file = aar_load_file,
	.release_file = aar_release_file,
	.copy_descriptor = NULL,
	.for_each = aar_for_each,
	.free_data = aar_free_data,
	.free = aar_free,
};

static bool aar_exists(struct archive *_ar, int no)
{
	struct aar_archive *ar = (struct aar_archive*)_ar;
	return (uint32_t)no < ar->nr_files;
}

static bool aar_inflate_entry(struct archive_data *data, uint8_t *buf, uint32_t size)
{
	if (memcmp(buf, "ZLB\0", 4))
		return false;
	uint32_t version = LittleEndian_getDW(buf, 4);
	if (version != 0) {
		WARNING("unknown ZLB version: %u", version);
		return false;
	}
	unsigned long out_size = LittleEndian_getDW(buf, 8);
	uint32_t in_size = LittleEndian_getDW(buf, 12);
	if (in_size + 16 > size) {
		WARNING("Bad ZLB size");
		return false;
	}
	uint8_t *out = xmalloc(out_size);
	if (uncompress(out, &out_size, buf + 16, in_size) != Z_OK) {
		WARNING("uncompress failed");
		free(out);
		return false;
	}
	data->data = out;
	data->size = out_size;
	return true;
}

static bool aar_load_file(struct archive_data *data)
{
	if (data->data)
		return true;

	struct aar_archive *ar = (struct aar_archive*)data->archive;
	struct aar_entry *e = &ar->files[data->no];

	while (e->type == AAR_SYMLINK) {
		e = ht_get_ignorecase(ar->ht, e->link_target, NULL);
		if (!e) {
			WARNING("orphaned symlink: %s", ar->files[data->no].name);
			return false;
		}
	}

	if (ar->ar.mmapped) {
		uint8_t *ptr = (uint8_t *)ar->mmap_ptr + e->off;
		if (e->type == AAR_COMPRESSED)
			return aar_inflate_entry(data, ptr, e->size);
		data->data = ptr;
		data->size = e->size;
		return true;
	}

	fseek(ar->f, e->off, SEEK_SET);
	uint8_t *buf = xmalloc(e->size);
	if (e->size > 0 && fread(buf, e->size, 1, ar->f) != 1) {
		WARNING("Failed to read '%s': %s", ar->filename, strerror(errno));
		free(buf);
		return false;
	}
	if (e->type == AAR_COMPRESSED) {
		bool result = aar_inflate_entry(data, buf, e->size);
		free(buf);
		return result;
	}

	data->data = buf;
	data->size = e->size;
	return true;
}

static struct archive_data *aar_get_descriptor(struct archive *_ar, int no)
{
	struct aar_archive *ar = (struct aar_archive*)_ar;
	if ((uint32_t)no >= ar->nr_files)
		return NULL;
	struct aar_entry *e = &ar->files[no];
	struct archive_data *data = xcalloc(1, sizeof(struct archive_data));
	data->size = e->size;  // Note: this may be compressed size
	data->name = e->name;
	data->no = no;
	data->archive = _ar;
	return data;
}

static struct archive_data *aar_get(struct archive *_ar, int no)
{
	struct archive_data *data = aar_get_descriptor(_ar, no);
	if (!data)
		return NULL;
	if (!aar_load_file(data)) {
		aar_free_data(data);
		return NULL;
	}
	return data;
}

static struct archive_data *aar_get_by_name(struct archive *_ar, const char *name)
{
	struct aar_archive *ar = (struct aar_archive*)_ar;
	struct aar_entry *e = ht_get_ignorecase(ar->ht, name, NULL);
	if (!e)
		return NULL;
	return aar_get(_ar, e - ar->files);
}

static void aar_release_file(struct archive_data *data)
{
	if (!data->data)
		return;
	struct aar_archive *ar = (struct aar_archive*)data->archive;
	uint8_t *mmap_ptr = ar->mmap_ptr;
	if (!(mmap_ptr && mmap_ptr <= data->data && data->data < mmap_ptr + ar->file_size))
		free(data->data);
	data->data = NULL;
}

static void aar_for_each(struct archive *_ar, void (*iter)(struct archive_data *data, void *user), void *user)
{
	struct aar_archive *ar = (struct aar_archive*)_ar;
	for (uint32_t i = 0; i < ar->nr_files; i++) {
		struct archive_data *data = aar_get_descriptor(_ar, i);
		if (!data)
			continue;
		iter(data, user);
		aar_free_data(data);
	}
}

static void aar_free_data(struct archive_data *data)
{
	aar_release_file(data);
	free(data);
}

static void aar_free(struct archive *_ar)
{
	struct aar_archive *ar = (struct aar_archive*)_ar;
	if (ar->mmap_ptr)
		munmap(ar->mmap_ptr, ar->file_size);
	if (ar->f)
		fclose(ar->f);
	ht_free(ar->ht);
	free(ar->filename);
	free(ar->files);
	free(ar->index_buf);
	free(ar);
}

static char *get_string(uint8_t **ptr, struct aar_archive *ar)
{
	const int key = ar->version >= 2 ? 0x60 : 0;
	char *str = (char *)*ptr;
	while (**ptr)
		*(*ptr)++ -= key;
	(*ptr)++;
	return str;
}

static bool aar_read_index(FILE *f, struct aar_archive *ar, int *error)
{
	uint8_t header[16];
	if (fread(header, 16, 1, f) != 1) {
		*error = ARCHIVE_FILE_ERROR;
		return false;
	}

	if (memcmp(header, "AAR\0", 4)) {
		*error = ARCHIVE_BAD_ARCHIVE_ERROR;
		return false;
	}
	ar->version = LittleEndian_getDW(header, 4);
	if (ar->version != 0 && ar->version != 2) {
		WARNING("Unknown AAR version %u", ar->version);
		*error = ARCHIVE_BAD_ARCHIVE_ERROR;
		return false;
	}
	ar->nr_files = LittleEndian_getDW(header, 8);
	uint32_t first_entry_offset = LittleEndian_getDW(header, 12);

	ar->index_buf = xmalloc(first_entry_offset);
	memcpy(ar->index_buf, header, 16);
	if (fread(ar->index_buf + 16, first_entry_offset - 16, 1, f) != 1) {
		free(ar->index_buf);
		*error = ARCHIVE_FILE_ERROR;
		return false;
	}

	ar->files = xcalloc(ar->nr_files, sizeof(struct aar_entry));
	ar->ht = ht_create(ar->nr_files * 3 / 2);
	uint8_t *p = ar->index_buf + 12;
	for (int i = 0; i < ar->nr_files; i++) {
		struct aar_entry *file = &ar->files[i];
		file->off = LittleEndian_getDW(p, 0);
		file->size = LittleEndian_getDW(p, 4);
		file->type = LittleEndian_getDW(p, 8);
		p += 12;
		file->name = get_string(&p, ar);
		if (ar->version >= 2)
			file->link_target = get_string(&p, ar);
		ht_put_ignorecase(ar->ht, file->name, file);
		if (p > ar->index_buf + first_entry_offset)
			break;
	}
	if (p != ar->index_buf + first_entry_offset) {
		WARNING("unexpected index size");
		ht_free(ar->ht);
		free(ar->files);
		free(ar->index_buf);
		*error = ARCHIVE_FILE_ERROR;
		return false;
	}

	fseek(f, 0, SEEK_END);
	ar->file_size = ftell(f);
	fseek(f, 0, SEEK_SET);
	return true;
}

struct aar_archive *aar_open(const char *file, int flags, int *error)
{
#ifdef _WIN32
	flags &= ~ARCHIVE_MMAP;
#endif
	FILE *fp;
	struct aar_archive *ar = xcalloc(1, sizeof(struct aar_archive));

	if (!(fp = file_open_utf8(file, "rb"))) {
		WARNING("fopen failed: %s", strerror(errno));
		*error = ARCHIVE_FILE_ERROR;
		goto exit_err;
	}
	if (!aar_read_index(fp, ar, error)) {
		WARNING("aar_read_index failed");
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
	ar->ar.ops = &aar_archive_ops;
	return ar;
exit_err:
	free(ar);
	return NULL;
}
