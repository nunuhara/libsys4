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
#include <stdint.h>
#include <string.h>
#include "little_endian.h"
#include "system4.h"
#include "system4/cg.h"
#include "system4/pcf.h"
#include "system4/qnt.h"

bool pcf_checkfmt(const uint8_t *data)
{
	return data[0] == 'p' && data[1] == 'c' && data[2] == 'f' && data[3] == ' ';
}

static size_t pcf_get_cg_offset(const uint8_t *data, size_t size, size_t *size_out)
{
	if (size < 8)
		return 0;

	// size of 'pcf ' section
	size_t pcf_size = LittleEndian_getDW(data, 4);

	if (size < pcf_size + 16)
		return 0;
	if (strncmp((const char*)data + 8 + pcf_size, "ptdl", 4))
		return 0;

	// size of 'ptdl' section
	size_t ptdl_size = LittleEndian_getDW(data, 8 + pcf_size + 4);

	if (size < pcf_size + ptdl_size + 24)
		return 0;
	if (strncmp((const char*)data + 8 + pcf_size + 8 + ptdl_size, "pcgd", 4))
		return 0;

	// size of 'pcgd' section (CG)
	size_t pcgd_size = LittleEndian_getDW(data, 8 + pcf_size + 8 + ptdl_size + 4);

	if (pcgd_size < 4 || strncmp((const char*)data + 8 + pcf_size + 8 + ptdl_size + 8, "QNT", 4)) {
		WARNING("pcf CG isn't qnt format");
		return 0;
	}

	*size_out = pcgd_size;
	return 8 + pcf_size + 8 + ptdl_size + 8;
}

bool pcf_get_metrics(const uint8_t *data, size_t size, struct cg_metrics *dst)
{
	size_t cg_size;
	size_t cg_offset = pcf_get_cg_offset(data, size, &cg_size);
	if (!cg_offset)
		return false;

	return qnt_get_metrics(data + cg_offset, dst);
}

void pcf_extract(const uint8_t *data, size_t size, struct cg *cg)
{
	size_t cg_size;
	size_t cg_offset = pcf_get_cg_offset(data, size, &cg_size);
	if (!cg_offset)
		return;

	return qnt_extract(data + cg_offset, cg);
}
