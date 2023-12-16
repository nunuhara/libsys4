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

#ifndef SYSTEM4_MT19937INT_H
#define SYSTEM4_MT19937INT_H

#include <stddef.h>
#include <stdint.h>

#define MT19937_STATE_SIZE 624

struct mt19937 {
	uint32_t st[MT19937_STATE_SIZE];
	int i;
};

void mt19937_init(struct mt19937 *mt, uint32_t seed);
uint32_t mt19937_genrand(struct mt19937 *mt);
void mt19937_xorcode(uint8_t *buf, size_t len, uint32_t seed);

#endif /* SYSTEM4_MT19937INT_H */
