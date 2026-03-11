#ifndef SNESRECOMP_FUNC_TABLE_H
#define SNESRECOMP_FUNC_TABLE_H

/*
 * Recompiled function dispatch table.
 *
 * Register your recompiled C functions at their original SNES 24-bit
 * addresses, then call them by address. All functions use the global
 * CPU state (g_cpu) and bus_read8/bus_write8 for memory access.
 */

#include <stdint.h>
#include <stdbool.h>

/* All recompiled functions: no args, no return, use globals */
typedef void (*snes_func_t)(void);

void func_table_init(void);
void func_table_register(uint32_t snes_addr, snes_func_t func);
bool func_table_call(uint32_t snes_addr);
snes_func_t func_table_lookup(uint32_t snes_addr);

#endif /* SNESRECOMP_FUNC_TABLE_H */
