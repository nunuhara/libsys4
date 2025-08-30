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
#include <zlib.h>
#include "little_endian.h"
#include "system4.h"
#include "system4/ajp.h"
#include "system4/archive.h"
#include "system4/buffer.h"
#include "system4/cg.h"
#include "system4/file.h"
#include "system4/flat.h"
#include "system4/string.h"

static struct string *buffer_read_pascal_string_align4(struct buffer *r)
{
	int32_t len = buffer_read_int32(r);
	if (len < 0 || buffer_remaining(r) < len) {
		ERROR("Invalid string length %d", len);
	}


	struct string *s = make_string(buffer_strdata(r), len);
	buffer_skip(r, len);
	buffer_align(r, 4);
	return s;
}


static const char *get_file_extension(int type, const char *data)
{
	switch (type) {
	case FLAT_CG:
		if (!strncmp(data, "AJP", 4))
			return ".ajp";
		if (!strncmp(data, "QNT", 4))
			return ".qnt";
		return ".img";
	case FLAT_ZLIB:
		return ".z.dat";
	default:
		return ".dat";
	}
}

static void flat_free_data(struct archive_data *data)
{
	struct flat_data *flat = (struct flat_data*)data;
	if (flat->inflated)
		free(data->data);
	free(data->name);
	free(data);
}

static void free_timeline(struct flat_timeline *tl)
{
	free_string(tl->name);
	free_string(tl->library_name);
	switch (tl->type) {
		case FLAT_TIMELINE_GRAPHIC:
			if (tl->graphic.keys) {
				// v < 15
				free(tl->graphic.keys);
			}
			if (tl->graphic.frames) {
				// v >= 15
				for (size_t i = 0; i < tl->frame_count; i++) {
					free(tl->graphic.frames[i].keys);
				}
				free(tl->graphic.frames);
			}

			break;
		case FLAT_TIMELINE_SCRIPT:
			if (tl->script.keys) {
				for (size_t i = 0; i < tl->script.count; i++) {
					if (tl->script.keys[i].text) free_string(tl->script.keys[i].text);
				}
				free(tl->script.keys);
			}
			break;
		default:
			break;
	}
}

static void flat_free(struct archive *_ar)
{
	struct flat_archive *ar = (struct flat_archive*)_ar;
	if (ar->needs_free)
		free(ar->data);
	for (size_t i = 0; i < ar->nr_mtlc_timelines; i++) {
		free_timeline(&ar->mtlc_timelines[i]);
	}
	free(ar->mtlc_timelines);
	free(ar->libl_entries);
	for (unsigned i = 0; i < ar->nr_talt_entries; i++) {
		free(ar->talt_entries[i].metadata);
	}
	free(ar->talt_entries);
	free(ar);
}

static bool flat_get_entry(struct flat_archive *ar, unsigned no, struct flat_data *dst)
{
	if (no < ar->nr_libl_entries) {
		dst->off = ar->libl_entries[no].off;
		dst->size = ar->libl_entries[no].size;
		dst->type = ar->libl_entries[no].type;
		return true;
	}

	no -= ar->nr_libl_entries;
	if (no < ar->nr_talt_entries) {
		dst->off = ar->talt_entries[no].off;
		dst->size = ar->talt_entries[no].size;
		dst->type = FLAT_CG;
		return true;
	}

	return false;
}

static struct archive_data *flat_get(struct archive *_ar, int no)
{
	struct flat_archive *ar = (struct flat_archive*)_ar;
	struct flat_data *data = xcalloc(1, sizeof(struct flat_data));

	if (!flat_get_entry(ar, no, data)) {
		free(data);
		return NULL;
	}

	const char *section = (unsigned)no < ar->nr_libl_entries ? "LIBL" : "TALT";

	data->super.data = ar->data + data->off;
	data->super.size = data->size;
	data->super.name = xmalloc(256);
	snprintf(data->super.name, 256, "%s_%d%s", section, no, get_file_extension(data->type, (const char*)ar->data+data->off));
	data->super.no = no;
	data->super.archive = _ar;
	return &data->super;
}

static bool flat_load_file(possibly_unused struct archive_data *data)
{
	struct flat_archive *ar = (struct flat_archive*)data->archive;
	struct flat_data *flatdata = (struct flat_data*)data;

	if (!data->data) {
		data->data = ar->data + flatdata->off;
	}

	// inflate zlib compressed data
	if (flatdata->type == FLAT_ZLIB && ar->data[flatdata->off+4] == 0x78) {
		unsigned long size = LittleEndian_getDW(ar->data, flatdata->off);
		uint8_t *out = xmalloc(size);
		if (uncompress(out, &size, ar->data + flatdata->off + 4, flatdata->size - 4) != Z_OK) {
			WARNING("uncompress failed");
			free(out);
			return false;
		}
		data->data = out;
		data->size = size;
		flatdata->inflated = true;
	}

	return true;
}

static void flat_release_file(possibly_unused struct archive_data *data)
{
	struct flat_archive *ar = (struct flat_archive*)data->archive;
	struct flat_data *flatdata = (struct flat_data*)data;
	if (flatdata->inflated) {
		free(data->data);
		data->data = ar->data + flatdata->off;
		data->size = flatdata->size;
		flatdata->inflated = false;
	}
}

static struct archive_data *flat_copy_descriptor(struct archive_data *_src)
{
	struct flat_data *src = (struct flat_data*)_src;
	struct flat_data *dst = xmalloc(sizeof(struct flat_data));
	_archive_copy_descriptor_ip(&dst->super, &src->super);
	dst->off = src->off;
	dst->size = src->size;
	dst->type = src->type;
	dst->inflated = false;
	return &dst->super;
}

static void flat_for_each(struct archive *_ar, void (*iter)(struct archive_data *data, void *user), void *user)
{
	struct flat_archive *ar = (struct flat_archive*)_ar;
	for (unsigned i = 0; i < ar->nr_libl_entries + ar->nr_talt_entries; i++) {
		struct archive_data *data = flat_get(_ar, i);
		if (!data)
			continue;
		iter(data, user);
		flat_free_data(data);
	}
}

static struct archive_ops flat_archive_ops = {
	.exists = NULL,
	.get = flat_get,
	.get_by_name = NULL,
	.get_by_basename = NULL,
	.load_file = flat_load_file,
	.release_file = flat_release_file,
	.copy_descriptor = flat_copy_descriptor,
	.for_each = flat_for_each,
	.free_data = flat_free_data,
	.free = flat_free,
};

static bool is_image(const char *data)
{
	if (!strncmp(data, "AJP", 4))
		return true;
	if (!strncmp(data, "QNT", 4))
		return true;
	return false;
}

static void read_libl(struct flat_archive *ar)
{
	if (!ar->libl.present)
		return;

	struct buffer r;
	buffer_init(&r, ar->data + ar->libl.off + 8, ar->libl.size);

	ar->nr_libl_entries = buffer_read_int32(&r);
	ar->libl_entries = xcalloc(ar->nr_libl_entries, sizeof(struct libl_entry));
	for (unsigned i = 0; i < ar->nr_libl_entries; i++) {
		ar->libl_entries[i].unknown_size = buffer_read_int32(&r);
		ar->libl_entries[i].unknown_off = ar->libl.off + r.index + 8;
		buffer_skip(&r, ar->libl_entries[i].unknown_size);
		buffer_align(&r, 4);

		ar->libl_entries[i].type = buffer_read_int32(&r);
		ar->libl_entries[i].size = buffer_read_int32(&r);
		ar->libl_entries[i].off = ar->libl.off + r.index + 8;

		if (ar->libl_entries[i].type == FLAT_CG && !is_image(buffer_strdata(&r))) {
			// XXX: special case: CG usually (not always!) has extra int32
			if (is_image(buffer_strdata(&r)+4)) {
				ar->libl_entries[i].off += 4;
				ar->libl_entries[i].size -= 4;
				ar->libl_entries[i].has_front_pad = true;
				ar->libl_entries[i].front_pad = buffer_read_int32(&r);
			} else {
				WARNING("Couldn't read CG data in LIBL section");
			}
		}

		buffer_skip(&r, ar->libl_entries[i].size);
		buffer_align(&r, 4);
	}

	if (r.index != ar->libl.size)
		WARNING("Junk at end of LIBL section");
}

static void read_talt(struct flat_archive *ar)
{
	if (!ar->talt.present)
		return;

	struct buffer r;
	buffer_init(&r, ar->data + ar->talt.off + 8, ar->talt.size);

	ar->nr_talt_entries = buffer_read_int32(&r);
	ar->talt_entries = xcalloc(ar->nr_talt_entries, sizeof(struct talt_entry));
	for (unsigned i = 0; i < ar->nr_talt_entries; i++) {
		ar->talt_entries[i].size = buffer_read_int32(&r);
		ar->talt_entries[i].off = ar->talt.off + r.index + 8;

		if (strncmp(buffer_strdata(&r), "AJP", 4))
			WARNING("File in flat TALT section is not ajp format");

		buffer_skip(&r, ar->talt_entries[i].size);
		buffer_align(&r, 4);

		ar->talt_entries[i].nr_meta = buffer_read_int32(&r);
		ar->talt_entries[i].metadata = xcalloc(ar->talt_entries[i].nr_meta, sizeof(struct talt_metadata));
		for (unsigned j = 0; j < ar->talt_entries[i].nr_meta; j++) {
			ar->talt_entries[i].metadata[j].unknown1_size = buffer_read_int32(&r);
			ar->talt_entries[i].metadata[j].unknown1_off = ar->talt.off + r.index + 8;
			buffer_skip(&r, ar->talt_entries[i].metadata[j].unknown1_size);
			buffer_align(&r, 4);

			ar->talt_entries[i].metadata[j].unknown2 = buffer_read_int32(&r);
			ar->talt_entries[i].metadata[j].unknown3 = buffer_read_int32(&r);
			ar->talt_entries[i].metadata[j].unknown4 = buffer_read_int32(&r);
			ar->talt_entries[i].metadata[j].unknown5 = buffer_read_int32(&r);
		}
	}

	if (r.index != ar->talt.size)
		WARNING("Junk at end of TALT section");
}

static void read_one_graphic_key(struct buffer *r, struct flat_key_data_graphic *out, const int version)
{
	if (version <= 4) {
		out->pos_x.i = buffer_read_int32(r);
		out->pos_y.i = buffer_read_int32(r);
	} else {
		out->pos_x.f = buffer_read_float(r);
		out->pos_y.f = buffer_read_float(r);
	}
	out->scale_x = buffer_read_float(r);
	out->scale_y = buffer_read_float(r);
	out->angle_x = buffer_read_float(r);
	out->angle_y = buffer_read_float(r);
	out->angle_z = buffer_read_float(r);
	out->add_r = buffer_read_int32(r);
	out->add_g = buffer_read_int32(r);
	out->add_b = buffer_read_int32(r);
	out->mul_r = buffer_read_int32(r);
	out->mul_g = buffer_read_int32(r);
	out->mul_b = buffer_read_int32(r);
	out->alpha = buffer_read_int32(r);
	out->area_x = buffer_read_int32(r);
	out->area_y = buffer_read_int32(r);
	out->area_width = buffer_read_int32(r);
	out->area_height = buffer_read_int32(r);
	out->draw_filter = buffer_read_int32(r);
	out->uk1 = (version > 8) ? buffer_read_int32(r) : 0;
	out->origin_x = buffer_read_int32(r);
	out->origin_y = buffer_read_int32(r);
	out->uk2 = (version > 7) ? buffer_read_int32(r) : 0;
	out->reverse_tb = buffer_read_int32(r) != 0;
	out->reverse_lr = buffer_read_int32(r) != 0;
}

static size_t graphic_key_data_size(const int version)
{
	size_t sz = 92;
	if ( version > 7) sz += 4;
	if ( version > 8) sz += 4;
	return sz;
}

static void read_graphic_tl(struct flat_timeline *tl, struct buffer *r, const int version)
{
	if (tl->frame_count <= 0) {
		WARNING("Timeline has no frames");
		return;
	}

	tl->graphic.count = 0;
	tl->graphic.keys = NULL;
	tl->graphic.frames = NULL;

	const size_t ksz = graphic_key_data_size(version);

	if (version < 15) {
		tl->graphic.count = tl->frame_count;
		tl->graphic.keys = xcalloc(tl->graphic.count, sizeof *tl->graphic.keys);

		for (size_t i = 0; i < tl->graphic.count; i++) {
			if (buffer_remaining(r) < ksz) {
				WARNING("Not enough data for graphic key %zu/%d (v<15)", i, tl->frame_count);
				tl->graphic.count = i;
				break;
			}
			read_one_graphic_key(r, &tl->graphic.keys[i], version);
		}

		return;
	}

	tl->graphic.frames = xcalloc(tl->frame_count, sizeof *tl->graphic.frames);

	for (size_t f = 0; f < tl->frame_count; f++) {
		uint32_t n = buffer_read_int32(r);

		tl->graphic.frames[f].count = n;

		size_t need = n *ksz;
		if (buffer_remaining(r) < need) {
			WARNING("Frame %zu declares %u keys (%zu bytes) but only %zu bytes remain; truncating.",
					f, n, need, buffer_remaining(r));
			n = (int32_t)(buffer_remaining(r) / ksz);
			tl->graphic.frames[f].count = n;
		}

		tl->graphic.frames[f].keys = xcalloc(n, sizeof *tl->graphic.frames[f].keys);
		for (size_t i = 0; i < n; i++) {
			read_one_graphic_key(r, &tl->graphic.frames[f].keys[i], version);
		}
	}
}

static void read_one_script_key(struct buffer *r, struct flat_script_key *out)
{
	out->frame_index = buffer_read_int32(r);
	out->has_jump = false;
	out->jump_frame = -1;
	out->is_stop = false;
	out->text = NULL;

	for (;;) {
		const int32_t op = buffer_read_int32(r);
		switch (op) {
			case 0: return;
			case 1:
				out->has_jump = true;
				out->jump_frame = buffer_read_int32(r);
				break;
			case 2:
				out->is_stop = true;
				break;
			case 3:
				out->text = buffer_read_pascal_string_align4(r);
				break;
			default:
				ERROR("Unknown script key operation %d", op);
		}
	}
}

static void read_script_tl(struct flat_timeline *tl, struct buffer *r)
{
	if (buffer_remaining(r) < 4) {
		WARNING("Not enough data for script timeline");
		return;
	}
	tl->script.count = buffer_read_int32(r);
	tl->script.keys = xcalloc(tl->script.count, sizeof(struct flat_script_key));

	for (size_t i = 0; i < tl->script.count; i++) {
		read_one_script_key(r, &tl->script.keys[i]);
	}
}

static bool parse_mtlc_payload(struct flat_archive *ar, uint8_t *data, size_t size)
{
	struct buffer r;
	buffer_init(&r, data, size);

	if (buffer_remaining(&r) < 4) {
		WARNING("Not enough data for MTLC header");
		return false;
	}

	ar->nr_mtlc_timelines = buffer_read_int32(&r);
	ar->mtlc_timelines = xcalloc(ar->nr_mtlc_timelines, sizeof(struct flat_timeline));

	for (size_t i = 0; i < ar->nr_mtlc_timelines; i++) {
		struct flat_timeline *tl = &ar->mtlc_timelines[i];
		tl->name = buffer_read_pascal_string_align4(&r);
		tl->library_name = buffer_read_pascal_string_align4(&r);
		tl->type = buffer_read_int32(&r);
		tl->begin_frame = buffer_read_int32(&r);
		tl->frame_count = buffer_read_int32(&r);

		switch (tl->type) {
			case FLAT_TIMELINE_GRAPHIC:
				read_graphic_tl(tl, &r, ar->fh.version);
				break;
			case FLAT_TIMELINE_SCRIPT:
				read_script_tl(tl, &r);
				break;
			case FLAT_TIMELINE_SOUND:
				WARNING("Unimplemented timeline SOUND");
				ar->nr_mtlc_timelines = i;
				return true; // Parsed as much as we could
			default:
				WARNING("Unknown MTLC timeline type %d", tl->type);
				ar->nr_mtlc_timelines = i;
				return true; // Parsed as much as we could
		}
	}

	if (r.index != size)
		WARNING("Junk at end of MTLC section");

	return true;
}


static void read_mtlc(struct flat_archive *ar)
{
	if (!ar->mtlc.present)
		return;
	if (!ar->fh.present) {
		WARNING("Cannot read MTLC section without valid FLAT header");
		return;
	}

	const bool is_compressed = ar->fh.version >= 4;

	uint8_t *payload = ar->data + ar->mtlc.off + 8;
	size_t payload_size = ar->mtlc.size;

	if (!is_compressed) {
		if (!parse_mtlc_payload(ar, payload, payload_size)) {
			WARNING("Failed to parse uncompressed MTLC section");
		}
		return;
	}

	struct buffer r;
	buffer_init(&r, payload, ar->mtlc.size);

	unsigned long uncompressed_size = buffer_read_int32(&r);

	uint8_t *out = xmalloc(uncompressed_size);
	int res = uncompress(out, &uncompressed_size, (uint8_t*)buffer_strdata(&r), ar->mtlc.size -4);
	if (res != Z_OK) {
		WARNING("uncompress failed for MTLC section");
		free(out);
		return;
	}
	if (!parse_mtlc_payload(ar, out, uncompressed_size)) {
		WARNING("Failed to parse compressed MTLC section");
	}
	free(out);
}

static void read_flat_hdr_v1(struct flat_archive *ar)
{
	if (!ar->flat.present) {
		WARNING("FLAT section not present in archive");
		ar->fh.present = false;
		return;
	}
	ar->fh.type = FLAT_HDR_V1_32;

	struct buffer r;
	buffer_init(&r, ar->data + ar->flat.off + 8, ar->flat.size);

	if (buffer_remaining(&r) < (int)(8 * 4)) {
		WARNING("FLAT section too small: %uB", (unsigned)buffer_remaining(&r));
		ar->fh.present = false;
		return;
	}

	ar->fh.fps = buffer_read_int32(&r);
	ar->fh.game_view_width = buffer_read_int32(&r);
	ar->fh.game_view_height = buffer_read_int32(&r);
	ar->fh.camera_length = buffer_read_float(&r);
	ar->fh.meter = buffer_read_float(&r);
	ar->fh.width = buffer_read_int32(&r);
	ar->fh.height = buffer_read_int32(&r);
	ar->fh.version = buffer_read_int32(&r);
	ar->fh.present = true;
}

static void read_flat_hdr_v2(struct flat_archive *ar)
{
	if (!ar->flat.present) {
		WARNING("FLAT section not present in archive");
		ar->fh.present = false;
		return;
	}
	ar->fh.type = FLAT_HDR_V2_64;

	struct buffer r;
	buffer_init(&r, ar->data + ar->flat.off + 8, ar->flat.size);

	if (buffer_remaining(&r) < (int)(9 * 4)) {
		WARNING("FLAT section too small: %uB", (unsigned)buffer_remaining(&r));
		ar->fh.present = false;
		return;
	}

	ar->fh.version = buffer_read_int32(&r);
	ar->fh.fps = buffer_read_int32(&r);
	ar->fh.game_view_width = buffer_read_int32(&r);
	ar->fh.game_view_height = buffer_read_int32(&r);
	ar->fh.camera_length = buffer_read_float(&r);
	ar->fh.meter = buffer_read_float(&r);
	ar->fh.width = buffer_read_int32(&r);
	ar->fh.height = buffer_read_int32(&r);
	ar->fh.uk1 = buffer_read_int32(&r);
	ar->fh.present = true;
}

static bool read_section(const char *magic, struct buffer *r, struct flat_section *dst)
{
	if (buffer_remaining(r) < 8)
		return false;
	if (strncmp(buffer_strdata(r), magic, 4))
		return false;
	dst->present = true;
	dst->off = r->index;
	buffer_skip(r, 4);
	dst->size = buffer_read_int32(r);
	buffer_skip(r, dst->size);
	return true;
}

struct flat_archive *flat_new(void)
{
	struct flat_archive *ar = xcalloc(1, sizeof(struct flat_archive));
	ar->ar.ops = &flat_archive_ops;
	return ar;
}

struct flat_archive *flat_open(uint8_t *data, size_t size, int *error)
{
	struct flat_archive *ar = flat_new();
	struct buffer r;
	buffer_init(&r, data, size);

	read_section("ELNA", &r, &ar->elna);
	if (!read_section("FLAT", &r, &ar->flat))
		goto bad_archive;
	read_section("TMNL", &r, &ar->tmnl);
	if (!read_section("MTLC", &r, &ar->mtlc))
		goto bad_archive;
	if (!read_section("LIBL", &r, &ar->libl))
		goto bad_archive;
	read_section("TALT", &r, &ar->talt);

	if (r.index < size)
		WARNING("Junk at end of FLAT file? %uB/%uB", (unsigned)r.index, (unsigned)size);
	else if (r.index > size)
		WARNING("FLAT file truncated? %uB/%uB", (unsigned)size, (unsigned)r.index);

	ar->data_size = size;
	ar->data = data;

	switch (ar->flat.size) {
		case 32: read_flat_hdr_v1(ar); break;
		case 64: read_flat_hdr_v2(ar); break;
		default:
			WARNING("Unknown FLAT header type with size %uB", (unsigned)ar->flat.size);
			ar->fh.present = false;
			ar->fh.type = FLAT_HDR_UNKNOWN;
			break;
	}

	read_mtlc(ar);
	read_libl(ar);
	read_talt(ar);

	return ar;

bad_archive:
	free(ar);
	*error = ARCHIVE_BAD_ARCHIVE_ERROR;
	return NULL;
}

struct flat_archive *flat_open_file(const char *path, possibly_unused int flags, int *error)
{
	size_t size;
	uint8_t *data = file_read(path, &size);
	if (!data) {
		*error = ARCHIVE_FILE_ERROR;
		return NULL;
	}

	struct flat_archive *ar = (struct flat_archive*)flat_open(data, size, error);
	if (!ar) {
		free(data);
		return NULL;
	}

	ar->needs_free = true;
	return ar;
}
