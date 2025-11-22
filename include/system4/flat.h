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


enum flat_error {
	FLAT_SUCCESS = 0,
	FLAT_BAD_SECTION = 1,
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

struct flat_stop_motion {
	struct string *library_name;
	int32_t span;
	int32_t loop_type;
};

struct flat_emitter {
	// uk stands for unknown field I don't know the meaning of it
	struct string *library_name;
	int32_t uk_int1;
	int32_t create_pos_type;
	float create_pos_length;
	float create_pos_length2;
	int32_t create_count;
	int32_t particle_length;
	float begin_size_rate;
	float uk1_size_rate;
	float end_size_rate;
	float uk2_size_rate;
	float begin_x_size_rate;
	float uk1_x_size_rate;
	float end_x_size_rate;
	float uk2_x_size_rate;
	float begin_y_size_rate;
	float uk1_y_size_rate;
	float end_y_size_rate;
	float uk2_y_size_rate;
	bool uk_bool1;
	int32_t direction_type;
	float direction_x;
	float direction_y;
	float direction_z;
	float direction_angle;
	bool is_emitter_connect_type;
	int32_t uk_int2;
	int32_t uk_int3;
	int32_t uk_int4;
	int32_t uk_int5;
	int32_t uk_int6;
	int32_t uk_int7;
	int32_t uk_int8;
	int32_t uk_int9;
	int32_t uk_int10;
	int32_t uk_int11;
	float speed;
	float speed_rate;
	float move_length;
	float mobe_curve;
	float uk_float1;
	bool is_fall;
	float width;
	float air_resistance;
	bool uk_bool2;
	float begin_x_angle;
	float uk1_x_angle;
	float end_x_angle;
	float uk2_x_angle;
	float begin_y_angle;
	float uk1_y_angle;
	float end_y_angle;
	float uk2_y_angle;
	float begin_z_angle;
	float uk1_z_angle;
	float end_z_angle;
	float uk2_z_angle;
	bool uk_bool3;
	int32_t fade_in_frame;
	int32_t fade_out_frame;
	int32_t draw_filter_type;
	int32_t rand_base;
	int32_t end_pos_type;
	float end_pos_x;
	float end_pos_y;
	float end_pos_z;
	struct string *end_cg_name;
};

enum flat_library_type {
	FLAT_LIB_CG = 2,
	FLAT_LIB_MEMORY = 4,
	FLAT_LIB_TIMELINE = 5,
	FLAT_LIB_STOP_MOTION = 6,
	FLAT_LIB_EMITTER = 7,
};

struct flat_library {
	struct string *name;
	enum flat_library_type type;
	size_t size;
	uint8_t *decoded; // only for ELNA-encrypted data.
	union {
		struct { const uint8_t *data; size_t size; int32_t uk_int; } cg;
		struct { struct flat_timeline *timelines; size_t nr_timelines; } timeline;
		struct flat_stop_motion stop_motion;
		struct flat_emitter emitter;
		// TODO: memory
	};
	// XXX: for alice-tools
	uint32_t off;
	uint32_t payload_off;
};


struct flat_section {
	bool present;
	size_t off;
	size_t size;
};

struct flat {
	// file map
	struct flat_section elna;
	struct flat_section flat;
	struct flat_section tmnl;
	struct flat_section mtlc;
	struct flat_section libl;
	struct flat_section talt;

	struct flat_header hdr;

	// From the LIBL section
	size_t nr_libraries;
	struct flat_library *libraries;

	// From the mtlc section
	size_t nr_timelines;
	struct flat_timeline *timelines;

	size_t nr_talt_entries;
	struct talt_entry *talt_entries;

	bool needs_free;
	size_t data_size;
	uint8_t *data;
};




struct flat *flat_open(uint8_t *data, size_t size, int *error);
void flat_free(struct flat *fl);

#endif /* SYSTEM4_FLAT_H */
