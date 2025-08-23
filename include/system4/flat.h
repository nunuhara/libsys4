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

#ifndef SYSTEM4_FLAT_H
#define SYSTEM4_FLAT_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "system4/archive.h"

enum flat_data_type {
	FLAT_CG  = 2,
	FLAT_ZLIB = 5,
};

struct libl_entry {
	uint32_t unknown_size;
	uint32_t unknown_off;
	enum flat_data_type type;
	bool has_front_pad;
	uint32_t front_pad;
	uint32_t size;
	uint32_t off;
};

struct talt_metadata {
	uint32_t unknown1_size;
	uint32_t unknown1_off;
	uint32_t unknown2;
	uint32_t unknown3;
	uint32_t unknown4;
	uint32_t unknown5;
};

struct talt_entry {
	uint32_t size;
	uint32_t off;
	uint32_t nr_meta;
	struct talt_metadata *metadata;
};

enum flat_header_type {
	FLAT_HDR_UNKNOWN,
	FLAT_HDR_V1_32, // 32-bytes
	FLAT_HDR_V2_64,
};

struct flat_header {
	bool present;

	enum flat_header_type type;

	// V2-only
	int32_t uk1;

	// Common fields
	int32_t fps;
	int32_t game_view_width;
	int32_t game_view_height;
	float camera_length;
	float meter;
	int32_t width;
	int32_t height;
	int32_t version;
};

enum flat_timeline_type {
	FLAT_TIMELINE_GRAPHIC = 3,
	FLAT_TIMELINE_SOUND = 4,
	FLAT_TIMELINE_SCRIPT = 5,
};

struct flat_key_data_graphic {
	// After version 4 position is stored as float
	union { int32_t i; float f; } pos_x;
	union { int32_t i; float f; } pos_y;
	float scale_x;
	float scale_y;
	float angle_x;
	float angle_y;
	float angle_z;
	int32_t add_r;
	int32_t add_g;
	int32_t add_b;
	int32_t mul_r;
	int32_t mul_g;
	int32_t mul_b;
	int32_t alpha;
	int32_t area_x;
	int32_t area_y;
	int32_t area_width;
	int32_t area_height;
	int32_t draw_filter;
	int32_t uk1; // only version > 8
	int32_t origin_x;
	int32_t origin_y;
	int32_t uk2; // Only version > 7
	bool reverse_tb; // Top/bottom
	bool reverse_lr; // Left/right
};

struct flat_key_frame_graphic {
	uint32_t count;
	struct flat_key_data_graphic *keys;
};

struct flat_graphic_timeline {
	// v < 15
	uint32_t count;
	struct flat_key_data_graphic *keys;

	// v >= 15
	struct flat_key_frame_graphic *frames;
};

struct flat_script_key {
	uint32_t frame_index;
	bool has_jump;
	int32_t jump_frame;
	bool is_stop;
	struct string *text;
};

struct flat_script_timeline {
	uint32_t count;
	struct flat_script_key *keys;
};


struct flat_timeline {
	struct string *name;
	struct string *library_name;
	enum flat_timeline_type type;
	int32_t begin_frame;
	int32_t frame_count;
	union {
		struct flat_graphic_timeline graphic;
		struct flat_script_timeline script;
	};
};

struct flat_data {
	struct archive_data super;
	size_t off;
	size_t size;
	enum flat_data_type type;
	bool inflated;
};

struct flat_section {
	bool present;
	size_t off;
	size_t size;
};

struct flat_archive {
	struct archive ar;

	// file map
	struct flat_section elna;
	struct flat_section flat;
	struct flat_section tmnl;
	struct flat_section mtlc;
	struct flat_section libl;
	struct flat_section talt;

	struct flat_header fh;

	uint32_t nr_libl_entries;
	struct libl_entry *libl_entries;
	uint32_t nr_talt_entries;
	struct talt_entry *talt_entries;
	uint32_t nr_mtlc_timelines;
	struct flat_timeline *mtlc_timelines;

	bool needs_free;
	size_t data_size;
	uint8_t *data;
};

struct flat_archive *flat_new(void);
struct flat_archive *flat_open(uint8_t *data, size_t size, int *error);
struct flat_archive *flat_open_file(const char *path, int flags, int *error);

#endif /* SYSTEM4_FLAT_H */
