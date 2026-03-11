#ifndef SNESRECOMP_CPU_H
#define SNESRECOMP_CPU_H

/*
 * 65C816 CPU register state for recompiled code.
 *
 * This struct is what your recompiled functions read/write directly.
 * It is NOT connected to LakeSnes's CPU (which we don't use) — the
 * recompiled native code IS the CPU. This struct just holds the
 * register state that the original 65816 code would have used.
 */

#include <stdint.h>
#include <stdbool.h>

typedef struct SnesCpu {
    /* Accumulator: C = (B << 8) | A in 16-bit mode */
    uint16_t C;
    uint16_t X;
    uint16_t Y;
    uint16_t S;   /* Stack pointer */
    uint16_t DP;  /* Direct Page register */
    uint8_t  DB;  /* Data Bank register */
    uint8_t  PB;  /* Program Bank register */

    /* Processor status flags (individual bools for fast branching) */
    bool flag_C;  /* Carry */
    bool flag_Z;  /* Zero */
    bool flag_I;  /* IRQ Disable */
    bool flag_D;  /* Decimal */
    bool flag_X;  /* Index register size (1=8-bit) */
    bool flag_M;  /* Accumulator size (1=8-bit) */
    bool flag_V;  /* Overflow */
    bool flag_N;  /* Negative */
    bool flag_E;  /* Emulation mode */
} SnesCpu;

extern SnesCpu g_cpu;

/* --- Accumulator access macros --- */
#define CPU_A8()       ((uint8_t)(g_cpu.C & 0xFF))
#define CPU_A16()      (g_cpu.C)
#define CPU_B()        ((uint8_t)(g_cpu.C >> 8))
#define CPU_SET_A8(v)  (g_cpu.C = (g_cpu.C & 0xFF00) | ((uint8_t)(v)))
#define CPU_SET_A16(v) (g_cpu.C = (uint16_t)(v))

/* --- Index register access (8-bit mode reads) --- */
#define CPU_X8()       ((uint8_t)(g_cpu.X & 0xFF))
#define CPU_Y8()       ((uint8_t)(g_cpu.Y & 0xFF))

/* --- Inline flag helpers --- */

static inline void cpu_update_nz8(uint8_t val) {
    g_cpu.flag_N = (val & 0x80) != 0;
    g_cpu.flag_Z = (val == 0);
}

static inline void cpu_update_nz16(uint16_t val) {
    g_cpu.flag_N = (val & 0x8000) != 0;
    g_cpu.flag_Z = (val == 0);
}

static inline uint8_t cpu_get_p(void) {
    uint8_t p = 0;
    if (g_cpu.flag_C) p |= 0x01;
    if (g_cpu.flag_Z) p |= 0x02;
    if (g_cpu.flag_I) p |= 0x04;
    if (g_cpu.flag_D) p |= 0x08;
    if (g_cpu.flag_X) p |= 0x10;
    if (g_cpu.flag_M) p |= 0x20;
    if (g_cpu.flag_V) p |= 0x40;
    if (g_cpu.flag_N) p |= 0x80;
    return p;
}

static inline void cpu_set_p(uint8_t p) {
    g_cpu.flag_C = (p & 0x01) != 0;
    g_cpu.flag_Z = (p & 0x02) != 0;
    g_cpu.flag_I = (p & 0x04) != 0;
    g_cpu.flag_D = (p & 0x08) != 0;
    g_cpu.flag_X = (p & 0x10) != 0;
    g_cpu.flag_M = (p & 0x20) != 0;
    g_cpu.flag_V = (p & 0x40) != 0;
    g_cpu.flag_N = (p & 0x80) != 0;
}

/* Reset CPU to power-on state (emulation mode) */
void recomp_cpu_reset(void);

#endif /* SNESRECOMP_CPU_H */
