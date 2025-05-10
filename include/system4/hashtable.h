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

#ifndef SYSTEM4_HASHTABLE_H
#define SYSTEM4_HASHTABLE_H

#include <stddef.h>

struct ht_slot {
	union {
		char *key;
		int ikey;
	};
	void *value;
};

struct hash_table;

struct hash_table *ht_create(size_t nr_buckets);
void ht_free(struct hash_table *ht);

void *ht_get(struct hash_table *ht, const char *key, void *dflt);
bool _ht_get(struct hash_table *ht, const char *key, void **out);
struct ht_slot *ht_put(struct hash_table *ht, const char *key, void *dflt);
void ht_foreach_value(struct hash_table *ht, void(*fun)(void*));
void ht_foreach(struct hash_table *ht, void(*fun)(struct ht_slot*, void*), void *data);

/*
 * Integer-keyed hash tables. These functions should not be used together
 * with the regular ht_get/ht_put functions on the same hash table.
 */
void *ht_get_int(struct hash_table *ht, int key, void *dflt);
struct ht_slot *ht_put_int(struct hash_table *ht, int key, void *dflt);
void ht_free_int(struct hash_table *ht);

#endif /* SYSTEM4_HASHTABLE_H */
