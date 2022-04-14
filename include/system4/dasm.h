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

#ifndef SYSTEM4_DASM_H
#define SYSTEM4_DASM_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

struct ain;
struct instruction;

#define DASM_FUNC_STACK_SIZE 16

struct dasm {
	struct ain *ain;
	uint32_t addr;
	int func;
	int func_stack[DASM_FUNC_STACK_SIZE];
	const struct instruction *instr;
};

void dasm_init(struct dasm *dasm, struct ain *ain);
struct dasm *dasm_open(struct ain *ain);
void dasm_close(struct dasm *dasm);
bool dasm_eof(struct dasm *dasm);
uint32_t dasm_addr(struct dasm *dasm);
const struct instruction *dasm_instruction(struct dasm *dasm);
void dasm_jump(struct dasm *dasm, uint32_t addr);
void dasm_next(struct dasm *dasm);
int dasm_peek(struct dasm *dasm);
int dasm_opcode(struct dasm *dasm);
int dasm_nr_args(struct dasm *dasm);
int32_t dasm_arg(struct dasm *dasm, int n);
int dasm_arg_type(struct dasm *dasm, int n);
int dasm_function(struct dasm *dasm);

#endif /* SYSTEM4_DASM_H */
