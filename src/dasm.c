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
#include <stdlib.h>

#include "system4.h"
#include "system4/ain.h"
#include "system4/dasm.h"
#include "system4/instructions.h"
#include "system4/little_endian.h"

void dasm_init(struct dasm *dasm, struct ain *ain)
{
	dasm->ain = ain;
	dasm_jump(dasm, 0);
}

struct dasm *dasm_open(struct ain *ain)
{
	struct dasm *dasm = xcalloc(1, sizeof(struct dasm));
	dasm_init(dasm, ain);
	return dasm;
}

void dasm_close(struct dasm *dasm)
{
	free(dasm);
}

bool dasm_eof(struct dasm *dasm)
{
	return dasm->addr >= dasm->ain->code_size;
}

uint32_t dasm_addr(struct dasm *dasm)
{
	return dasm->addr;
}

const struct instruction *dasm_instruction(struct dasm *dasm)
{
	return dasm->instr;
}

static const struct instruction *dasm_get_instruction(struct dasm *dasm)
{
	uint16_t opcode = LittleEndian_getW(dasm->ain->code, dasm->addr) & ~OPTYPE_MASK;
	return opcode < NR_OPCODES ? &instructions[opcode] : &instructions[0];
}

static void dasm_enter_function(struct dasm *dasm, int fno)
{
	for (int i = 1; i < DASM_FUNC_STACK_SIZE; i++) {
		dasm->func_stack[i] = dasm->func_stack[i-1];
	}
	dasm->func_stack[0] = dasm->func;
	dasm->func = fno;
}

static void dasm_leave_function(struct dasm *dasm)
{
	dasm->func = dasm->func_stack[0];
	for (int i = 1; i < DASM_FUNC_STACK_SIZE; i++) {
		dasm->func_stack[i-1] = dasm->func_stack[i];
	}
}

static void dasm_update(struct dasm *dasm)
{
	dasm->instr = dasm_eof(dasm) ? &instructions[0] : dasm_get_instruction(dasm);
	if (dasm->instr->opcode == FUNC)
		dasm_enter_function(dasm, dasm_arg(dasm, 0));
	else if (dasm->instr->opcode == ENDFUNC)
		dasm_leave_function(dasm);
}

void dasm_jump(struct dasm *dasm, uint32_t addr)
{
	dasm->addr = addr;
	dasm_update(dasm);
}

void dasm_next(struct dasm *dasm)
{
	dasm->addr += instruction_width(dasm->instr->opcode);
	dasm_update(dasm);
}

int dasm_peek(struct dasm *dasm)
{
	int width = instruction_width(dasm->instr->opcode);
	if (dasm->addr+width >= dasm->ain->code_size)
		return -1;
	return LittleEndian_getW(dasm->ain->code, dasm->addr+width);
}

int dasm_opcode(struct dasm *dasm)
{
	return dasm->instr->opcode;
}

int dasm_nr_args(struct dasm *dasm)
{
	return dasm->instr->nr_args;
}

int32_t dasm_arg(struct dasm *dasm, int n)
{
	if (n < 0 || n >= dasm->instr->nr_args)
		return 0;
	return LittleEndian_getDW(dasm->ain->code, dasm->addr + 2 + 4*n);
}

int dasm_arg_type(struct dasm *dasm, int n)
{
	if (n < 0 || n >= dasm->instr->nr_args)
		return 0;
	return dasm->instr->args[n];
}

int dasm_function(struct dasm *dasm)
{
	return dasm->func;
}
