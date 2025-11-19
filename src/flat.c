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
#include "system4/buffer.h"
#include "system4/file.h"
#include "system4/flat.h"
#include "system4/string.h"
#include "system4/utfsjis.h"

static struct string *buffer_read_flat_string(struct buffer *r)
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

static void free_library(struct flat_library *lib)
{

	switch (lib->type) {

		case FLAT_LIB_TIMELINE:
			for (size_t i = 0; i < lib->timeline.nr_timelines; i++) {
				free_timeline(&lib->timeline.timelines[i]);
			}
			free(lib->timeline.timelines);
			break;
		case FLAT_LIB_STOP_MOTION:
			free_string(lib->stop_motion.library_name);
			break;
		case FLAT_LIB_EMITTER:
			free_string(lib->emitter.library_name);
			free_string(lib->emitter.end_cg_name);
			break;
		case FLAT_LIB_CG:
		case FLAT_LIB_MEMORY:
		default:
			break;
	}

	free_string(lib->name);
	if (lib->decoded)
		free(lib->decoded);

}

void flat_free(struct flat *fl)
{
	if (fl->needs_free)
		free(fl->data);
	for (size_t i = 0; i < fl->nr_timelines; i++) {
		free_timeline(&fl->timelines[i]);
	}
	free(fl->timelines);
	for (size_t i = 0; i < fl->nr_libraries; i++) {
		free_library(&fl->libraries[i]);
	}
	free(fl->libraries);
	for (unsigned i = 0; i < fl->nr_talt_entries; i++) {
		free(fl->talt_entries[i].metadata);
	}
	free(fl->talt_entries);
	free(fl);
}


static void read_talt(struct flat *fl)
{
	if (!fl->talt.present)
		return;

	struct buffer r;
	buffer_init(&r, fl->data + fl->talt.off + 8, fl->talt.size);

	fl->nr_talt_entries = buffer_read_int32(&r);
	fl->talt_entries = xcalloc(fl->nr_talt_entries, sizeof(struct talt_entry));
	for (unsigned i = 0; i < fl->nr_talt_entries; i++) {
		fl->talt_entries[i].size = buffer_read_int32(&r);
		fl->talt_entries[i].off = fl->talt.off + r.index + 8;

		if (strncmp(buffer_strdata(&r), "AJP", 4) != 0)
			WARNING("File in flat TALT section is not ajp format");

		buffer_skip(&r, fl->talt_entries[i].size);
		buffer_align(&r, 4);

		fl->talt_entries[i].nr_meta = buffer_read_int32(&r);
		fl->talt_entries[i].metadata = xcalloc(fl->talt_entries[i].nr_meta, sizeof(struct talt_metadata));
		for (unsigned j = 0; j < fl->talt_entries[i].nr_meta; j++) {
			fl->talt_entries[i].metadata[j].unknown1_size = buffer_read_int32(&r);
			fl->talt_entries[i].metadata[j].unknown1_off = fl->talt.off + r.index + 8;
			buffer_skip(&r, fl->talt_entries[i].metadata[j].unknown1_size);
			buffer_align(&r, 4);

			fl->talt_entries[i].metadata[j].unknown2 = buffer_read_int32(&r);
			fl->talt_entries[i].metadata[j].unknown3 = buffer_read_int32(&r);
			fl->talt_entries[i].metadata[j].unknown4 = buffer_read_int32(&r);
			fl->talt_entries[i].metadata[j].unknown5 = buffer_read_int32(&r);
		}
	}

	if (r.index != fl->talt.size)
		WARNING("Junk at end of TALT section");
}

static void parse_graphic_key(struct buffer *r, struct flat_key_data_graphic *out, const int version)
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
			parse_graphic_key(r, &tl->graphic.keys[i], version);
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
			parse_graphic_key(r, &tl->graphic.frames[f].keys[i], version);
		}
	}
}

static void parse_script_key(struct buffer *r, struct flat_script_key *out)
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
				out->text = buffer_read_flat_string(r);
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
		parse_script_key(r, &tl->script.keys[i]);
	}
}

static bool parse_timeline(struct flat *fl, struct flat_timeline *tl, struct buffer *r)
{
	tl->name = buffer_read_flat_string(r);
	tl->library_name = buffer_read_flat_string(r);
	tl->type = buffer_read_int32(r);
	tl->begin_frame = buffer_read_int32(r);
	tl->frame_count = buffer_read_int32(r);

	switch (tl->type) {
		case FLAT_TIMELINE_GRAPHIC:
			read_graphic_tl(tl, r, fl->hdr.version);
			break;
		case FLAT_TIMELINE_SCRIPT:
			read_script_tl(tl, r);
			break;
		case FLAT_TIMELINE_SOUND:
			WARNING("Unimplemented timeline SOUND");
			return false;
		default:
			WARNING("Unknown MTLC timeline type %d", tl->type);
			return false;
	}

	return true;
}


static bool parse_timelines(struct flat *fl,  struct buffer *r, struct flat_timeline *timelines[], size_t *nr_timelines)
{
	const bool is_compressed = fl->hdr.version >= 4;
	uint8_t *allocated = NULL;


	if (is_compressed) {
		unsigned long uncompressed_size = buffer_read_int32(r);

		allocated = xmalloc(uncompressed_size);
		int res = uncompress(allocated, &uncompressed_size, buffer_data(r), buffer_remaining(r));
		if (res != Z_OK) {
			WARNING("Failed to uncompress timelines");
			goto error;
		}

		buffer_init(r, allocated, uncompressed_size);
	}

	*nr_timelines = buffer_read_int32(r);
	*timelines = xcalloc(*nr_timelines, sizeof(struct flat_timeline));

	for (size_t i = 0; i < *nr_timelines; i++) {
		if (!parse_timeline(fl, &(*timelines)[i], r)) {
			WARNING("Failed to parse timeline %zu", i);
			*nr_timelines = i;
			goto error;
		}
	}

	if (allocated)
		free(allocated);

	return true;

error:
	if (allocated)
		free(allocated);
	return false;
}

static void read_mtlc(struct flat *fl)
{
	if (!fl->mtlc.present)
		return;
	if (!fl->hdr.present) {
		WARNING("Cannot read MTLC section without valid FLAT header");
		return;
	}

	struct buffer r;
	buffer_init(&r, fl->data + fl->mtlc.off + 8, fl->mtlc.size);

	if (!parse_timelines(fl, &r, &fl->timelines, &fl->nr_timelines)) {
		WARNING("Failed to parse MTLC timelines");
	}
}

static void parse_stop_motion(struct buffer *r, struct flat_stop_motion *sm)
{
	sm->library_name = buffer_read_flat_string(r);
	sm->span = buffer_read_int32(r);
	sm->loop_type = buffer_read_int32(r);
}

static void parse_emitter(struct buffer *r, struct flat_emitter *em, int version)
{
	em->library_name = buffer_read_flat_string(r);
	em->uk_int1 = version > 0 ? buffer_read_int32(r) : 5;
	em->create_pos_type = buffer_read_int32(r);
	em->create_pos_length = buffer_read_float(r);
	em->create_pos_length2 = buffer_read_float(r);
	em->create_count = buffer_read_int32(r);
	em->particle_length = buffer_read_int32(r);
	em->begin_size_rate = buffer_read_float(r);
	if (version < 1) {
		em->end_size_rate = buffer_read_float(r);
		em->begin_x_size_rate = buffer_read_float(r);
		em->end_x_size_rate = buffer_read_float(r);
		em->begin_y_size_rate = buffer_read_float(r);
		em->end_y_size_rate = buffer_read_float(r);
	} else {
		em->uk1_size_rate = buffer_read_float(r);
		em->end_size_rate = buffer_read_float(r);
		em->uk2_size_rate = buffer_read_float(r);
		em->begin_x_size_rate = buffer_read_float(r);
		em->uk1_x_size_rate = buffer_read_float(r);
		em->end_x_size_rate = buffer_read_float(r);
		em->uk2_x_size_rate = buffer_read_float(r);
		em->begin_y_size_rate = buffer_read_float(r);
		em->uk1_y_size_rate = buffer_read_float(r);
		em->end_y_size_rate = buffer_read_float(r);
		em->uk2_y_size_rate = buffer_read_float(r);
		if (version > 5) {
			em->uk_bool1 = buffer_read_int32(r) != 0;
		}
	}
	em->direction_type = buffer_read_int32(r);
	em->direction_x = buffer_read_float(r);
	em->direction_y = buffer_read_float(r);
	em->direction_z = buffer_read_float(r);
	em->direction_angle = buffer_read_float(r);
	// TODO: Research maybe not bool in later versions sometimes not 0/1 just int
	em->is_emitter_connect_type = buffer_read_int32(r) != 0;
	if (version > 2) {
		em->uk_int2 = buffer_read_int32(r);
	}
	if (version > 9) {
		em->uk_int3 = buffer_read_int32(r);
	}
	if (version > 1) {
		em->uk_int4 = buffer_read_int32(r);
		em->uk_int5 = buffer_read_int32(r);
		em->uk_int6 = buffer_read_int32(r);
		em->uk_int7 = buffer_read_int32(r);
		em->uk_int8 = buffer_read_int32(r);
		em->uk_int9 = buffer_read_int32(r);
		em->uk_int10 = buffer_read_int32(r);
		em->uk_int11 = buffer_read_int32(r);
	}
	em->speed = buffer_read_float(r);
	em->speed_rate = buffer_read_float(r);
	em->move_length = buffer_read_float(r);
	em->mobe_curve = buffer_read_float(r);
	if (version > 1) {
		em->uk_float1 = buffer_read_float(r);
	}
	em->is_fall = buffer_read_int32(r) != 0;
	em->width = buffer_read_float(r);
	em->air_resistance = buffer_read_float(r);
	if (version > 1) {
		em->uk_bool2 = buffer_read_int32(r) != 0;
	}
	em->begin_x_angle = buffer_read_float(r);
	if (version < 1) {
		em->end_x_angle = buffer_read_float(r);
		em->begin_y_angle = buffer_read_float(r);
		em->end_y_angle = buffer_read_float(r);
		em->begin_z_angle = buffer_read_float(r);
		em->end_z_angle = buffer_read_float(r);
	} else {
		em->uk1_x_angle = buffer_read_float(r);
		em->end_x_angle = buffer_read_float(r);
		em->uk2_x_angle = buffer_read_float(r);
		em->begin_y_angle = buffer_read_float(r);
		em->uk1_y_angle = buffer_read_float(r);
		em->end_y_angle = buffer_read_float(r);
		em->uk2_y_angle = buffer_read_float(r);
		em->begin_z_angle = buffer_read_float(r);
		em->uk1_z_angle = buffer_read_float(r);
		em->end_z_angle = buffer_read_float(r);
		em->uk2_z_angle = buffer_read_float(r);
		if (version > 5) {
			em->uk_bool3 = buffer_read_int32(r) != 0;
		}
	}
	em->fade_in_frame = buffer_read_int32(r);
	em->fade_out_frame = buffer_read_int32(r);
	em->draw_filter_type = buffer_read_int32(r);
	em->rand_base = buffer_read_int32(r);
	em->end_pos_type = buffer_read_int32(r);
	em->end_pos_x = buffer_read_float(r);
	em->end_pos_y = buffer_read_float(r);
	em->end_pos_z = buffer_read_float(r);
	em->end_cg_name = buffer_read_flat_string(r);
}


static bool parse_library(struct flat *fl, struct flat_library *lib, struct buffer *r)
{
	if (fl->elna.present) {
		size_t size = buffer_read_int32(r);
		if (buffer_remaining(r) < size) {
			ERROR("Invalid ELNA string size %d", size);
		}

		uint8_t *src = buffer_data(r);
		uint8_t *tmp = xmalloc(size);
		for (size_t i = 0; i < size; i++) {
			tmp[i] = src[i] ^ 0x55;
		}

		lib->name = make_string((char*)tmp, size);
		free(tmp);

		buffer_skip(r, size);
		buffer_align(r, 4);
	} else {
		lib->name = buffer_read_flat_string(r);
	}
	lib->type = buffer_read_int32(r);
	lib->size = buffer_read_int32(r);


	if (buffer_remaining(r) < lib->size) {
		WARNING("LIBL entry has invalid size %zd while parsing library '%s'",
		        lib->size,
		        sjis2utf(lib->name->text, lib->name->size));
		return false;
	}

	struct buffer r_payload;
	buffer_init(&r_payload, buffer_data(r), lib->size);

	if (fl->elna.present &&
	(lib->type == FLAT_LIB_STOP_MOTION || lib->type == FLAT_LIB_EMITTER)) {
		if (buffer_remaining(&r_payload) < lib->size) {
			ERROR("Invalid ELNA string size %d", lib->size);
		}

		lib->decoded = xmalloc(lib->size);
		memcpy(lib->decoded, buffer_data(&r_payload), lib->size);
		for (size_t i = 0; i < lib->size; i++) {
			lib->decoded[i] ^= 0x55;
		}

		buffer_init(&r_payload, lib->decoded, lib->size);
	}


	switch (lib->type) {
		case FLAT_LIB_CG:
			if (fl->hdr.version > 0) {
				// No idea what this extra data is it reads a single int32 that is often 0 but sometimes 1
				// Probably some kind of metadata
				buffer_skip(&r_payload, 4);
			}
			lib->cg.data = buffer_data(&r_payload);
			lib->cg.size = lib->size;
			break;
		case FLAT_LIB_MEMORY:
			WARNING("FLAT_LIB_MEMORY not implemented");
			return false;
		case FLAT_LIB_TIMELINE:
			if (!parse_timelines(fl, &r_payload, &lib->timeline.timelines, &lib->timeline.nr_timelines)) {
				WARNING("Failed to parse LIBL timeline library '%s'",
				        sjis2utf(lib->name->text, lib->name->size));
				return false;
			}
			break;
		case FLAT_LIB_STOP_MOTION:
			parse_stop_motion(&r_payload, &lib->stop_motion);
			break;
		case FLAT_LIB_EMITTER:
			parse_emitter(&r_payload, &lib->emitter, fl->hdr.version);
			break;
		default:
			WARNING("Unknown LIBL entry type %d", lib->type);
			return false;
	}

	buffer_skip(r, lib->size);
	buffer_align(r, 4);


	return true;
}


static void read_libl(struct flat *fl)
{
	if (!fl->libl.present)
		return;

	struct buffer r;
	buffer_init(&r, fl->data + fl->libl.off + 8, fl->libl.size);

	fl->nr_libraries = buffer_read_int32(&r);
	fl->libraries = xcalloc(fl->nr_libraries, sizeof(struct flat_library));
	for (unsigned i = 0; i < fl->nr_libraries; i++) {
		if (!parse_library(fl, &fl->libraries[i], &r)) {
			WARNING("Failed to parse LIBL library %d", i);
			fl->nr_libraries = i;
			break;
		}

	}

	if (r.index != fl->libl.size)
		WARNING("Junk at end of LIBL section");
}



static void read_flat_hdr_v1(struct flat *fl)
{
	if (!fl->flat.present) {
		WARNING("FLAT section not present in archive");
		fl->hdr.present = false;
		return;
	}
	fl->hdr.type = FLAT_HDR_V1_32;

	struct buffer r;
	buffer_init(&r, fl->data + fl->flat.off + 8, fl->flat.size);

	if (buffer_remaining(&r) < 8 * 4) {
		WARNING("FLAT section too small: %uB", (unsigned)buffer_remaining(&r));
		fl->hdr.present = false;
		return;
	}

	fl->hdr.fps = buffer_read_int32(&r);
	fl->hdr.game_view_width = buffer_read_int32(&r);
	fl->hdr.game_view_height = buffer_read_int32(&r);
	fl->hdr.camera_length = buffer_read_float(&r);
	fl->hdr.meter = buffer_read_float(&r);
	fl->hdr.width = buffer_read_int32(&r);
	fl->hdr.height = buffer_read_int32(&r);
	fl->hdr.version = buffer_read_int32(&r);
	fl->hdr.present = true;
}

static void read_flat_hdr_v2(struct flat *ar)
{
	if (!ar->flat.present) {
		WARNING("FLAT section not present in archive");
		ar->hdr.present = false;
		return;
	}
	ar->hdr.type = FLAT_HDR_V2_64;

	struct buffer r;
	buffer_init(&r, ar->data + ar->flat.off + 8, ar->flat.size);

	if (buffer_remaining(&r) < (int)(9 * 4)) {
		WARNING("FLAT section too small: %uB", (unsigned)buffer_remaining(&r));
		ar->hdr.present = false;
		return;
	}

	ar->hdr.version = buffer_read_int32(&r);
	ar->hdr.fps = buffer_read_int32(&r);
	ar->hdr.game_view_width = buffer_read_int32(&r);
	ar->hdr.game_view_height = buffer_read_int32(&r);
	ar->hdr.camera_length = buffer_read_float(&r);
	ar->hdr.meter = buffer_read_float(&r);
	ar->hdr.width = buffer_read_int32(&r);
	ar->hdr.height = buffer_read_int32(&r);
	ar->hdr.uk1 = buffer_read_int32(&r);
	ar->hdr.present = true;
}

static bool read_section(const char *magic, struct buffer *r, struct flat_section *dst)
{
	if (buffer_remaining(r) < 8)
		return false;
	if (strncmp(buffer_strdata(r), magic, 4) != 0)
		return false;
	dst->present = true;
	dst->off = r->index;
	buffer_skip(r, 4);
	dst->size = buffer_read_int32(r);
	buffer_skip(r, dst->size);
	return true;
}


struct flat *flat_open(uint8_t *data, size_t size, int *error)
{
	struct flat *fl = xcalloc(1, sizeof(struct flat));
	struct buffer r;
	buffer_init(&r, data, size);

	read_section("ELNA", &r, &fl->elna);
	if (!read_section("FLAT", &r, &fl->flat))
		goto bad_section;
	read_section("TMNL", &r, &fl->tmnl);
	if (!read_section("MTLC", &r, &fl->mtlc))
		goto bad_section;
	if (!read_section("LIBL", &r, &fl->libl))
		goto bad_section;
	read_section("TALT", &r, &fl->talt);

	if (r.index < size)
		WARNING("Junk at end of FLAT file? %uB/%uB", (unsigned)r.index, (unsigned)size);
	else if (r.index > size)
		WARNING("FLAT file truncated? %uB/%uB", (unsigned)size, (unsigned)r.index);

	fl->data_size = size;
	fl->data = data;

	switch (fl->flat.size) {
		case 32: read_flat_hdr_v1(fl); break;
		case 64: read_flat_hdr_v2(fl); break;
		default:
			WARNING("Unknown FLAT header type with size %uB", (unsigned)fl->flat.size);
			fl->hdr.present = false;
			fl->hdr.type = FLAT_HDR_UNKNOWN;
			break;
	}

	read_mtlc(fl);
	read_libl(fl);
	read_talt(fl);

	return fl;

bad_section:
	free(fl);
	*error = FLAT_BAD_SECTION;
	return NULL;
}

