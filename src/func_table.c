/*
 * Recompiled function dispatch table.
 * Hash table mapping 24-bit SNES addresses to native C function pointers.
 */

#include "snesrecomp/func_table.h"
#include <string.h>
#include <stdio.h>

#define FUNC_TABLE_SIZE  4096  /* Must be power of 2 */

typedef struct FuncEntry {
    uint32_t    snes_addr;
    snes_func_t func;
    bool        occupied;
} FuncEntry;

static FuncEntry s_table[FUNC_TABLE_SIZE];

void func_table_init(void) {
    memset(s_table, 0, sizeof(s_table));
}

static uint32_t hash_addr(uint32_t addr) {
    addr = ((addr >> 16) ^ addr) * 0x45D9F3B;
    addr = ((addr >> 16) ^ addr) * 0x45D9F3B;
    addr = (addr >> 16) ^ addr;
    return addr & (FUNC_TABLE_SIZE - 1);
}

void func_table_register(uint32_t snes_addr, snes_func_t func) {
    uint32_t idx = hash_addr(snes_addr);
    for (uint32_t i = 0; i < FUNC_TABLE_SIZE; i++) {
        uint32_t slot = (idx + i) & (FUNC_TABLE_SIZE - 1);
        if (!s_table[slot].occupied || s_table[slot].snes_addr == snes_addr) {
            s_table[slot].snes_addr = snes_addr;
            s_table[slot].func = func;
            s_table[slot].occupied = true;
            return;
        }
    }
    fprintf(stderr, "func_table: table full, cannot register $%06X\n", snes_addr);
}

snes_func_t func_table_lookup(uint32_t snes_addr) {
    uint32_t idx = hash_addr(snes_addr);
    for (uint32_t i = 0; i < FUNC_TABLE_SIZE; i++) {
        uint32_t slot = (idx + i) & (FUNC_TABLE_SIZE - 1);
        if (!s_table[slot].occupied) return NULL;
        if (s_table[slot].snes_addr == snes_addr) return s_table[slot].func;
    }
    return NULL;
}

bool func_table_call(uint32_t snes_addr) {
    if (!recomp_interp_force()) {
        snes_func_t fn = func_table_lookup(snes_addr);
        if (fn) {
            fn();
            return true;
        }
    }
    if (recomp_interp_enabled()) {
        recomp_interp_call(snes_addr, /*is_long=*/true);
        return true;
    }
    fprintf(stderr, "func_table: no function at $%06X\n", snes_addr);
    return false;
}

bool func_table_call_jsr(uint32_t snes_addr) {
    if (!recomp_interp_force()) {
        snes_func_t fn = func_table_lookup(snes_addr);
        if (fn) {
            fn();
            return true;
        }
    }
    if (recomp_interp_enabled()) {
        recomp_interp_call(snes_addr, /*is_long=*/false);
        return true;
    }
    fprintf(stderr, "func_table: no function at $%06X\n", snes_addr);
    return false;
}
