/*
 * Recompiled function dispatch table.
 * Hash table mapping 24-bit SNES addresses to native C function pointers.
 */

#include "snesrecomp/func_table.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define FUNC_TABLE_SIZE  4096  /* Must be power of 2 */

/* Recursion guard for recompiled->recompiled dispatch. A recompiled function
 * that calls func_table_call (e.g. an indirect/jump-table dispatcher) into
 * another recompiled function recurses on the C stack; a dispatch cycle would
 * overflow it. Past a depth limit we route the call through the interpreter
 * instead, which runs on the EMULATED CPU stack (and is itself bounded by
 * INTERP_MAX_OPS), breaking the C-recursion without dropping the call's effects.
 * Tunable via SMK_RECOMP_MAXDEPTH; 0 disables the guard. */
static int s_call_depth = 0;
static int recomp_max_depth(void) {
    static int d = -1;
    if (d < 0) { const char *e = getenv("SMK_RECOMP_MAXDEPTH"); d = e ? atoi(e) : 48; }
    return d;
}

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

static bool func_table_dispatch(uint32_t snes_addr, bool is_long) {
    int maxd = recomp_max_depth();
    /* Below the depth limit, run the native recompiled body (C call). At/above
     * it, fall through to the interpreter so deep/cyclic dispatch unwinds on the
     * emulated stack instead of the C stack. */
    if (!recomp_interp_force() && (maxd == 0 || s_call_depth < maxd)) {
        snes_func_t fn = func_table_lookup(snes_addr);
        if (fn) {
            s_call_depth++;
            fn();
            s_call_depth--;
            return true;
        }
    }
    if (recomp_interp_enabled()) {
        recomp_interp_call(snes_addr, is_long);
        return true;
    }
    fprintf(stderr, "func_table: no function at $%06X\n", snes_addr);
    return false;
}

bool func_table_call(uint32_t snes_addr)     { return func_table_dispatch(snes_addr, true);  }
bool func_table_call_jsr(uint32_t snes_addr) { return func_table_dispatch(snes_addr, false); }
