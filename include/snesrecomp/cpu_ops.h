/*
 * 65C816 instruction helpers for recompiled code.
 *
 * Each macro/inline function implements a single 65816 instruction using
 * the global CPU state (g_cpu) and bus_read8/bus_write8. Recompiled
 * functions are sequences of these operations.
 *
 * This header is the public 65816 op kit — game-agnostic. Any recomp
 * project targeting a SNES ROM can use it as a starting set; extend with
 * project-specific helpers as needed.
 */
#ifndef SNESRECOMP_CPU_OPS_H
#define SNESRECOMP_CPU_OPS_H

#include "cpu.h"
#include "bus.h"

/* ========== Flag Operations ========== */

/* SEI / CLI */
#define OP_SEI()  (g_cpu.flag_I = true)
#define OP_CLI()  (g_cpu.flag_I = false)
#define OP_SEC()  (g_cpu.flag_C = true)
#define OP_CLC()  (g_cpu.flag_C = false)
#define OP_SED()  (g_cpu.flag_D = true)
#define OP_CLD()  (g_cpu.flag_D = false)
#define OP_CLV()  (g_cpu.flag_V = false)

/* REP #imm — clear specified flags */
static inline void op_rep(uint8_t val) {
    uint8_t p = cpu_get_p();
    p &= ~val;
    cpu_set_p(p);
}

/* SEP #imm — set specified flags */
static inline void op_sep(uint8_t val) {
    uint8_t p = cpu_get_p();
    p |= val;
    cpu_set_p(p);
}

/* XCE — exchange carry and emulation */
static inline void op_xce(void) {
    bool old_c = g_cpu.flag_C;
    g_cpu.flag_C = g_cpu.flag_E;
    g_cpu.flag_E = old_c;
    if (g_cpu.flag_E) {
        g_cpu.flag_M = true;
        g_cpu.flag_X = true;
        g_cpu.X &= 0xFF;
        g_cpu.Y &= 0xFF;
        g_cpu.S = (g_cpu.S & 0xFF) | 0x0100;
    }
}

/* XBA — exchange B and A (high/low bytes of C) */
static inline void op_xba(void) {
    g_cpu.C = (g_cpu.C >> 8) | (g_cpu.C << 8);
    cpu_update_nz8((uint8_t)(g_cpu.C & 0xFF));
}

/* ========== Load / Store ========== */

/* LDA immediate (8-bit) */
static inline void op_lda_imm8(uint8_t val) {
    CPU_SET_A8(val);
    cpu_update_nz8(val);
}

/* LDA immediate (16-bit) */
static inline void op_lda_imm16(uint16_t val) {
    CPU_SET_A16(val);
    cpu_update_nz16(val);
}

/* LDA dp (8-bit) */
static inline void op_lda_dp8(uint8_t dp) {
    uint16_t addr = g_cpu.DP + dp;
    uint8_t val = bus_wram_read8(addr);
    CPU_SET_A8(val);
    cpu_update_nz8(val);
}

/* LDA dp (16-bit) */
static inline void op_lda_dp16(uint8_t dp) {
    uint16_t addr = g_cpu.DP + dp;
    uint16_t val = bus_wram_read16(addr);
    CPU_SET_A16(val);
    cpu_update_nz16(val);
}

/* LDA abs (8-bit) */
static inline void op_lda_abs8(uint16_t addr) {
    uint8_t val = bus_read8(g_cpu.DB, addr);
    CPU_SET_A8(val);
    cpu_update_nz8(val);
}

/* LDA abs (16-bit) */
static inline void op_lda_abs16(uint16_t addr) {
    uint16_t val = bus_read16(g_cpu.DB, addr);
    CPU_SET_A16(val);
    cpu_update_nz16(val);
}

/* LDA long (16-bit) */
static inline void op_lda_long16(uint8_t bank, uint16_t addr) {
    uint16_t val = bus_read16(bank, addr);
    CPU_SET_A16(val);
    cpu_update_nz16(val);
}

/* LDX immediate (8-bit) */
static inline void op_ldx_imm8(uint8_t val) {
    g_cpu.X = val;
    cpu_update_nz8(val);
}

/* LDX immediate (16-bit) */
static inline void op_ldx_imm16(uint16_t val) {
    g_cpu.X = val;
    cpu_update_nz16(val);
}

/* LDX dp (16-bit) */
static inline void op_ldx_dp16(uint8_t dp) {
    uint16_t addr = g_cpu.DP + dp;
    g_cpu.X = bus_wram_read16(addr);
    cpu_update_nz16(g_cpu.X);
}

/* LDY immediate (16-bit) */
static inline void op_ldy_imm16(uint16_t val) {
    g_cpu.Y = val;
    cpu_update_nz16(val);
}

/* STA abs (8-bit) */
static inline void op_sta_abs8(uint16_t addr) {
    bus_write8(g_cpu.DB, addr, CPU_A8());
}

/* STA abs (16-bit) */
static inline void op_sta_abs16(uint16_t addr) {
    bus_write16(g_cpu.DB, addr, CPU_A16());
}

/* STA dp (8-bit) */
static inline void op_sta_dp8(uint8_t dp) {
    bus_wram_write8(g_cpu.DP + dp, CPU_A8());
}

/* STA dp (16-bit) */
static inline void op_sta_dp16(uint8_t dp) {
    bus_wram_write16(g_cpu.DP + dp, CPU_A16());
}

/* STA long (8-bit) */
static inline void op_sta_long8(uint8_t bank, uint16_t addr) {
    bus_write8(bank, addr, CPU_A8());
}

/* STA long (16-bit) */
static inline void op_sta_long16(uint8_t bank, uint16_t addr) {
    bus_write16(bank, addr, CPU_A16());
}

/* STX abs (16-bit) */
static inline void op_stx_abs16(uint16_t addr) {
    bus_write16(g_cpu.DB, addr, g_cpu.X);
}

/* STZ abs (8-bit) */
static inline void op_stz_abs8(uint16_t addr) {
    bus_write8(g_cpu.DB, addr, 0);
}

/* STZ abs (16-bit) */
static inline void op_stz_abs16(uint16_t addr) {
    bus_write16(g_cpu.DB, addr, 0);
}

/* STZ dp (8-bit) */
static inline void op_stz_dp8(uint8_t dp) {
    bus_wram_write8(g_cpu.DP + dp, 0);
}

/* STZ dp (16-bit) */
static inline void op_stz_dp16(uint8_t dp) {
    bus_wram_write16(g_cpu.DP + dp, 0);
}

/* ========== Transfer ========== */

#define OP_TCS()  (g_cpu.S = g_cpu.C)
#define OP_TSC()  (g_cpu.C = g_cpu.S)
#define OP_TCD()  (g_cpu.DP = g_cpu.C)
#define OP_TDC()  (g_cpu.C = g_cpu.DP)

static inline void op_tax(void) {
    g_cpu.X = g_cpu.flag_X ? (g_cpu.C & 0xFF) : g_cpu.C;
    if (g_cpu.flag_X) cpu_update_nz8((uint8_t)g_cpu.X);
    else cpu_update_nz16(g_cpu.X);
}

static inline void op_tay(void) {
    g_cpu.Y = g_cpu.flag_X ? (g_cpu.C & 0xFF) : g_cpu.C;
    if (g_cpu.flag_X) cpu_update_nz8((uint8_t)g_cpu.Y);
    else cpu_update_nz16(g_cpu.Y);
}

static inline void op_txa(void) {
    if (g_cpu.flag_M) { CPU_SET_A8((uint8_t)g_cpu.X); cpu_update_nz8(CPU_A8()); }
    else { CPU_SET_A16(g_cpu.X); cpu_update_nz16(g_cpu.X); }
}

static inline void op_tya(void) {
    if (g_cpu.flag_M) { CPU_SET_A8((uint8_t)g_cpu.Y); cpu_update_nz8(CPU_A8()); }
    else { CPU_SET_A16(g_cpu.Y); cpu_update_nz16(g_cpu.Y); }
}

/* ========== PHK / PLB (common pattern for setting DB) ========== */

/* PHK/PLB — set DB = bank (used as PHK;PLB or PHB;PHK;PLB) */
#define OP_SET_DB(bank)  (g_cpu.DB = (bank))

/* ========== Increment / Decrement ========== */

static inline void op_inc_dp8(uint8_t dp) {
    uint16_t addr = g_cpu.DP + dp;
    uint8_t val = bus_wram_read8(addr) + 1;
    bus_wram_write8(addr, val);
    cpu_update_nz8(val);
}

static inline void op_inc_dp16(uint8_t dp) {
    uint16_t addr = g_cpu.DP + dp;
    uint16_t val = bus_wram_read16(addr) + 1;
    bus_wram_write16(addr, val);
    cpu_update_nz16(val);
}

static inline void op_dec_a16(void) {
    uint16_t val = CPU_A16() - 1;
    CPU_SET_A16(val);
    cpu_update_nz16(val);
}

static inline void op_dec_a8(void) {
    uint8_t val = CPU_A8() - 1;
    CPU_SET_A8(val);
    cpu_update_nz8(val);
}

/* ========== Compare ========== */

static inline void op_cmp_imm8(uint8_t val) {
    uint16_t result = CPU_A8() - val;
    g_cpu.flag_C = (CPU_A8() >= val);
    g_cpu.flag_Z = ((result & 0xFF) == 0);
    g_cpu.flag_N = ((result & 0x80) != 0);
}

static inline void op_cmp_imm16(uint16_t val) {
    uint32_t result = CPU_A16() - val;
    g_cpu.flag_C = (CPU_A16() >= val);
    g_cpu.flag_Z = ((result & 0xFFFF) == 0);
    g_cpu.flag_N = ((result & 0x8000) != 0);
}

/* ========== Arithmetic ========== */

static inline void op_adc_imm16(uint16_t val) {
    uint32_t result = CPU_A16() + val + (g_cpu.flag_C ? 1 : 0);
    g_cpu.flag_V = (~(CPU_A16() ^ val) & (CPU_A16() ^ (uint16_t)result) & 0x8000) != 0;
    g_cpu.flag_C = (result > 0xFFFF);
    CPU_SET_A16((uint16_t)result);
    cpu_update_nz16(CPU_A16());
}

static inline void op_sbc_imm16(uint16_t val) {
    uint32_t result = CPU_A16() - val - (g_cpu.flag_C ? 0 : 1);
    g_cpu.flag_V = ((CPU_A16() ^ val) & (CPU_A16() ^ (uint16_t)result) & 0x8000) != 0;
    g_cpu.flag_C = (result <= 0xFFFF);
    CPU_SET_A16((uint16_t)result);
    cpu_update_nz16(CPU_A16());
}

/* AND immediate */
static inline void op_and_imm16(uint16_t val) {
    CPU_SET_A16(CPU_A16() & val);
    cpu_update_nz16(CPU_A16());
}

static inline void op_and_imm8(uint8_t val) {
    CPU_SET_A8(CPU_A8() & val);
    cpu_update_nz8(CPU_A8());
}

/* ========== Stack push/pull for register save/restore ========== */

static inline void op_php(void) {
    bus_wram_write8(g_cpu.S, cpu_get_p());
    g_cpu.S--;
}

static inline void op_plp(void) {
    g_cpu.S++;
    cpu_set_p(bus_wram_read8(g_cpu.S));
}

static inline void op_pha16(void) {
    g_cpu.S--;
    bus_wram_write16(g_cpu.S, CPU_A16());
    g_cpu.S--;
}

static inline void op_pla16(void) {
    g_cpu.S++;
    CPU_SET_A16(bus_wram_read16(g_cpu.S));
    g_cpu.S++;
    cpu_update_nz16(CPU_A16());
}

static inline void op_phx16(void) {
    g_cpu.S--;
    bus_wram_write16(g_cpu.S, g_cpu.X);
    g_cpu.S--;
}

static inline void op_plx16(void) {
    g_cpu.S++;
    g_cpu.X = bus_wram_read16(g_cpu.S);
    g_cpu.S++;
    cpu_update_nz16(g_cpu.X);
}

static inline void op_phy16(void) {
    g_cpu.S--;
    bus_wram_write16(g_cpu.S, g_cpu.Y);
    g_cpu.S--;
}

static inline void op_ply16(void) {
    g_cpu.S++;
    g_cpu.Y = bus_wram_read16(g_cpu.S);
    g_cpu.S++;
    cpu_update_nz16(g_cpu.Y);
}

static inline void op_phb(void) {
    bus_wram_write8(g_cpu.S, g_cpu.DB);
    g_cpu.S--;
}

static inline void op_plb(void) {
    g_cpu.S++;
    g_cpu.DB = bus_wram_read8(g_cpu.S);
    cpu_update_nz8(g_cpu.DB);
}

#endif /* SNESRECOMP_CPU_OPS_H */
