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

#ifndef SYSTEM4_ARCHIVE_H
#define SYSTEM4_ARCHIVE_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

enum ald_error {
	ARCHIVE_SUCCESS,
	ARCHIVE_FILE_ERROR,
	ARCHIVE_BAD_ARCHIVE_ERROR,
	ARCHIVE_MAX_ERROR
};

enum {
	ARCHIVE_MMAP = 1
};

struct archive {
	bool mmapped;
	struct archive_ops *ops;
	struct string *(*conv)(const char*,size_t);
};

struct archive_ops {
	bool (*exists)(struct archive *ar, int no);
	bool (*exists_by_name)(struct archive *ar, const char *name, int *id_out);
	bool (*exists_by_basename)(struct archive *ar, const char *name, int *id_out);
	struct archive_data *(*get)(struct archive *ar, int no);
	struct archive_data *(*get_by_name)(struct archive *ar, const char *name);
	struct archive_data *(*get_by_basename)(struct archive *ar, const char *name);
	bool (*load_file)(struct archive_data *file);
	void (*release_file)(struct archive_data *file);
	struct archive_data *(*copy_descriptor)(struct archive_data *src);
	void (*for_each)(struct archive *ar, void (*iter)(struct archive_data *data, void *user), void *user);
	void (*free_data)(struct archive_data *data);
	void (*free)(struct archive *ar);
};

struct archive_data {
	size_t size;
	uint8_t *data;
	char *name;
	int no;
	struct archive *archive;
};

/*
 * Returns a human readable description of an error.
 */
const char *archive_strerror(int error);

/*
 * Check if data exists in an archive.
 */
static inline bool archive_exists(struct archive *ar, int no)
{
	return ar->ops->exists ? ar->ops->exists(ar, no) : false;
}

/*
 * Check if data exists in an archive by name.
 */
static inline bool archive_exists_by_name(struct archive *ar, const char *name, int *id_out)
{
	return ar->ops->exists_by_name ? ar->ops->exists_by_name(ar, name, id_out) : false;
}

/*
 * Check if data exists in an archive by basename (i.e. ignoring file extension and case).
 */
static inline bool archive_exists_by_basename(struct archive *ar, const char *name, int *id_out)
{
	return ar->ops->exists_by_basename ? ar->ops->exists_by_basename(ar, name, id_out) : false;
}

/*
 * Retrieve a file from an archive by ID.
 */
static inline struct archive_data *archive_get(struct archive *ar, int no)
{
	return ar->ops->get ? ar->ops->get(ar, no) : NULL;
}

/*
 * Retrieve a file from an archive by name.
 */
static inline struct archive_data *archive_get_by_name(struct archive *ar, const char *name)
{
	return ar->ops->get_by_name ? ar->ops->get_by_name(ar, name) : NULL;
}

/*
 * Retrive a file from an archive by basename (i.e. ignoring file extension and case).
 */
static inline struct archive_data *archive_get_by_basename(struct archive *ar, const char *name)
{
	return ar->ops->get_by_basename ? ar->ops->get_by_basename(ar, name) : NULL;
}

/*
 * Load a file into memory, given an unloaded descriptor.
 * This should be used in conjunction with archive_for_each.
 */
static inline bool archive_load_file(struct archive_data *data)
{
	return data->archive->ops->load_file ? data->archive->ops->load_file(data) : false;
}

/*
 * Release file data loaded with `archive_load_file`.
 */
void _archive_release_file(struct archive_data *data);
static inline void archive_release_file(struct archive_data *data)
{
	if (data->archive->ops->release_file)
		data->archive->ops->release_file(data);
	else
		_archive_release_file(data);
}

/*
 * Copy a descriptor. This should be used in conjunction with `archive_for_each`
 * when descriptors will escape the iterator. A descriptor copied with this
 * function is initially unloaded, even if the source descriptor was loaded.
 */
struct archive_data *_archive_copy_descriptor(struct archive_data *src);
void _archive_copy_descriptor_ip(struct archive_data *dst, struct archive_data *src);
static inline struct archive_data *archive_copy_descriptor(struct archive_data *src)
{
	if (src->archive->ops->copy_descriptor)
		return src->archive->ops->copy_descriptor(src);
	return _archive_copy_descriptor(src);
}

/*
 * Iterate over all descriptors in the archive. Descriptors passed to `iter` are
 * initially unloaded, and should be loaded with `archive_load_file`.
 * Descriptors are closed after `iter` returns. If descriptors should escape the
 * scope of the iterator, they can be copied with `archive_copy_descriptor`.
 */
static inline void archive_for_each(struct archive *ar, void (*iter)(struct archive_data *data, void *user), void *user)
{
	if (ar->ops->for_each)
		ar->ops->for_each(ar, iter, user);
}

/*
 * Free an archive_data structure returned by `archive_get`.
 */
static inline void archive_free_data(struct archive_data *data)
{
	data->archive->ops->free_data(data);
}

/*
 * Free an ald_archive structure returned by `ald_open`.
 */
static inline void archive_free(struct archive *ar)
{
	ar->ops->free(ar);
}

struct archive_data *_archive_make_descriptor(struct archive *ar, char *name, int no, size_t size);

char *archive_basename(const char *name);

#endif /* SYSTEM4_ARCHIVE_H */
