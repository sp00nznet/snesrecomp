/*
 * Recomp CPU state — the 65816 registers your recompiled code uses.
 *
 * This is completely separate from LakeSnes's CPU emulation. In a
 * recomp, YOUR native C code IS the CPU. This struct just holds the
 * register file so recompiled instructions can read/write A, X, Y,
 * flags, etc.
 */

#include "snesrecomp/cpu.h"
#include <string.h>

SnesCpu g_cpu;

void recomp_cpu_reset(void) {
    memset(&g_cpu, 0, sizeof(g_cpu));

    /* Power-on defaults: emulation mode */
    g_cpu.flag_E = true;
    g_cpu.flag_M = true;   /* 8-bit accumulator */
    g_cpu.flag_X = true;   /* 8-bit index */
    g_cpu.flag_I = true;   /* IRQs disabled */
    g_cpu.S  = 0x01FF;     /* Stack at top of page 1 */
    g_cpu.DP = 0x0000;
    g_cpu.DB = 0x00;
    g_cpu.PB = 0x00;
}
