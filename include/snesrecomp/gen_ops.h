#ifndef SNESRECOMP_GEN_OPS_H
#define SNESRECOMP_GEN_OPS_H

/*
 * Runtime helpers for code emitted by tools/autogen.py.
 *
 * The generator translates each 65816 instruction into a call or two from
 * here, so the semantics live in one reviewed place rather than in thousands
 * of lines of emitted C. Everything operates on g_cpu and the bus exactly as
 * the CPU would: 8-bit operations keep the high byte of A, 8-bit index mode
 * keeps X/Y high bytes at zero, flags follow each instruction's rules, and
 * the stack lives in bank $00.
 *
 * Effective addresses are 24-bit (bank << 16 | addr). Indexed modes add with
 * carry into the bank, as the CPU does; direct page and stack-relative modes
 * stay in bank $00. A 16-bit access reads/writes ea and ea+1 within the bank.
 */

#include <stdint.h>
#include <stdbool.h>
#include "cpu.h"
#include "bus.h"

/* ---- memory ------------------------------------------------------------ */
static inline uint8_t  g_rd8(uint32_t ea)  { return bus_read8((uint8_t)(ea >> 16), (uint16_t)ea); }
static inline uint16_t g_rd16(uint32_t ea) {
    return (uint16_t)(g_rd8(ea) | (g_rd8((ea & 0xFF0000) | (uint16_t)(ea + 1)) << 8));
}
static inline void g_wr8(uint32_t ea, uint8_t v) { bus_write8((uint8_t)(ea >> 16), (uint16_t)ea, v); }
static inline void g_wr16(uint32_t ea, uint16_t v) {
    g_wr8(ea, (uint8_t)v);
    g_wr8((ea & 0xFF0000) | (uint16_t)(ea + 1), (uint8_t)(v >> 8));
}
static inline uint32_t g_rd24(uint32_t ea) {
    return (uint32_t)g_rd16(ea) | ((uint32_t)g_rd8((ea & 0xFF0000) | (uint16_t)(ea + 2)) << 16);
}

/* ---- effective addresses ---------------------------------------------- */
#define G_DB  ((uint32_t)g_cpu.DB << 16)
static inline uint32_t g_dp(uint8_t d)            { return (uint16_t)(g_cpu.DP + d); }
static inline uint32_t g_dpx(uint8_t d)           { return (uint16_t)(g_cpu.DP + d + g_cpu.X); }
static inline uint32_t g_dpy(uint8_t d)           { return (uint16_t)(g_cpu.DP + d + g_cpu.Y); }
static inline uint32_t g_abs(uint16_t a)          { return G_DB | a; }
static inline uint32_t g_absx(uint16_t a)         { return ((G_DB | a) + g_cpu.X) & 0xFFFFFF; }
static inline uint32_t g_absy(uint16_t a)         { return ((G_DB | a) + g_cpu.Y) & 0xFFFFFF; }
static inline uint32_t g_longx(uint32_t l)        { return (l + g_cpu.X) & 0xFFFFFF; }
static inline uint32_t g_dpi(uint8_t d)           { return G_DB | g_rd16(g_dp(d)); }
static inline uint32_t g_dpiy(uint8_t d)          { return ((G_DB | g_rd16(g_dp(d))) + g_cpu.Y) & 0xFFFFFF; }
static inline uint32_t g_dpxi(uint8_t d)          { return G_DB | g_rd16(g_dpx(d)); }
static inline uint32_t g_dpil(uint8_t d)          { return g_rd24(g_dp(d)); }
static inline uint32_t g_dpily(uint8_t d)         { return (g_rd24(g_dp(d)) + g_cpu.Y) & 0xFFFFFF; }
static inline uint32_t g_sr(uint8_t d)            { return (uint16_t)(g_cpu.S + d); }
static inline uint32_t g_sriy(uint8_t d)          { return ((G_DB | g_rd16(g_sr(d))) + g_cpu.Y) & 0xFFFFFF; }

/* ---- flags and registers ---------------------------------------------- */
static inline void g_nz8(uint8_t v)   { g_cpu.flag_N = (v & 0x80) != 0;   g_cpu.flag_Z = v == 0; }
static inline void g_nz16(uint16_t v) { g_cpu.flag_N = (v & 0x8000) != 0; g_cpu.flag_Z = v == 0; }

static inline void g_set_p(uint8_t p) {
    cpu_set_p(p);
    if (g_cpu.flag_X) { g_cpu.X &= 0xFF; g_cpu.Y &= 0xFF; }
}
static inline void g_rep(uint8_t v) { g_set_p((uint8_t)(cpu_get_p() & ~v)); }
static inline void g_sep(uint8_t v) { g_set_p((uint8_t)(cpu_get_p() | v)); }

static inline void g_seta8(uint8_t v)   { g_cpu.C = (uint16_t)((g_cpu.C & 0xFF00) | v); }
static inline void g_setidx(uint16_t *r, uint16_t v, bool wide) { *r = wide ? v : (uint16_t)(v & 0xFF); }

/* ---- stack (bank $00) --------------------------------------------------- */
static inline void    g_push8(uint8_t v)  { bus_write8(0x00, g_cpu.S, v); g_cpu.S--; }
static inline uint8_t g_pull8(void)       { g_cpu.S++; return bus_read8(0x00, g_cpu.S); }
static inline void    g_push16(uint16_t v){ g_push8((uint8_t)(v >> 8)); g_push8((uint8_t)v); }
static inline uint16_t g_pull16(void)     { uint8_t lo = g_pull8(); return (uint16_t)(lo | (g_pull8() << 8)); }

/* ---- arithmetic: ADC/SBC mirror LakeSnes cpu_adc/cpu_sbc, decimal mode included */
static inline uint8_t g_adc8(uint8_t a, uint8_t value) {
    int c = g_cpu.flag_C ? 1 : 0, result;
    if (g_cpu.flag_D) {
        result = (a & 0xf) + (value & 0xf) + c;
        if (result > 0x9) result = ((result + 0x6) & 0xf) + 0x10;
        result = (a & 0xf0) + (value & 0xf0) + result;
    } else {
        result = a + value + c;
    }
    g_cpu.flag_V = (a & 0x80) == (value & 0x80) && (value & 0x80) != (result & 0x80);
    if (g_cpu.flag_D && result > 0x9f) result += 0x60;
    g_cpu.flag_C = result > 0xff;
    g_nz8((uint8_t)result);
    return (uint8_t)result;
}
static inline uint16_t g_adc16(uint16_t a, uint16_t value) {
    int c = g_cpu.flag_C ? 1 : 0, result;
    if (g_cpu.flag_D) {
        result = (a & 0xf) + (value & 0xf) + c;
        if (result > 0x9) result = ((result + 0x6) & 0xf) + 0x10;
        result = (a & 0xf0) + (value & 0xf0) + result;
        if (result > 0x9f) result = ((result + 0x60) & 0xff) + 0x100;
        result = (a & 0xf00) + (value & 0xf00) + result;
        if (result > 0x9ff) result = ((result + 0x600) & 0xfff) + 0x1000;
        result = (a & 0xf000) + (value & 0xf000) + result;
    } else {
        result = a + value + c;
    }
    g_cpu.flag_V = (a & 0x8000) == (value & 0x8000) && (value & 0x8000) != (result & 0x8000);
    if (g_cpu.flag_D && result > 0x9fff) result += 0x6000;
    g_cpu.flag_C = result > 0xffff;
    g_nz16((uint16_t)result);
    return (uint16_t)result;
}
static inline uint8_t g_sbc8(uint8_t a, uint8_t b) {
    uint8_t value = (uint8_t)(b ^ 0xff);
    int c = g_cpu.flag_C ? 1 : 0, result;
    if (g_cpu.flag_D) {
        result = (a & 0xf) + (value & 0xf) + c;
        if (result < 0x10) result = (result - 0x6) & ((result - 0x6 < 0) ? 0xf : 0x1f);
        result = (a & 0xf0) + (value & 0xf0) + result;
    } else {
        result = a + value + c;
    }
    g_cpu.flag_V = (a & 0x80) == (value & 0x80) && (value & 0x80) != (result & 0x80);
    if (g_cpu.flag_D && result < 0x100) result -= 0x60;
    g_cpu.flag_C = result > 0xff;
    g_nz8((uint8_t)result);
    return (uint8_t)result;
}
static inline uint16_t g_sbc16(uint16_t a, uint16_t b) {
    uint16_t value = (uint16_t)(b ^ 0xffff);
    int c = g_cpu.flag_C ? 1 : 0, result;
    if (g_cpu.flag_D) {
        result = (a & 0xf) + (value & 0xf) + c;
        if (result < 0x10) result = (result - 0x6) & ((result - 0x6 < 0) ? 0xf : 0x1f);
        result = (a & 0xf0) + (value & 0xf0) + result;
        if (result < 0x100) result = (result - 0x60) & ((result - 0x60 < 0) ? 0xff : 0x1ff);
        result = (a & 0xf00) + (value & 0xf00) + result;
        if (result < 0x1000) result = (result - 0x600) & ((result - 0x600 < 0) ? 0xfff : 0x1fff);
        result = (a & 0xf000) + (value & 0xf000) + result;
    } else {
        result = a + value + c;
    }
    g_cpu.flag_V = (a & 0x8000) == (value & 0x8000) && (value & 0x8000) != (result & 0x8000);
    if (g_cpu.flag_D && result < 0x10000) result -= 0x6000;
    g_cpu.flag_C = result > 0xffff;
    g_nz16((uint16_t)result);
    return (uint16_t)result;
}
static inline void g_cmp8(uint8_t a, uint8_t b)    { g_cpu.flag_C = a >= b; g_nz8((uint8_t)(a - b)); }
static inline void g_cmp16(uint16_t a, uint16_t b) { g_cpu.flag_C = a >= b; g_nz16((uint16_t)(a - b)); }

/* ---- shifts (return the result, set C/N/Z) ------------------------------ */
static inline uint8_t  g_asl8(uint8_t v)   { g_cpu.flag_C = v >> 7;   v = (uint8_t)(v << 1);  g_nz8(v);  return v; }
static inline uint16_t g_asl16(uint16_t v) { g_cpu.flag_C = v >> 15;  v = (uint16_t)(v << 1); g_nz16(v); return v; }
static inline uint8_t  g_lsr8(uint8_t v)   { g_cpu.flag_C = v & 1;    v = (uint8_t)(v >> 1);  g_nz8(v);  return v; }
static inline uint16_t g_lsr16(uint16_t v) { g_cpu.flag_C = v & 1;    v = (uint16_t)(v >> 1); g_nz16(v); return v; }
static inline uint8_t  g_rol8(uint8_t v)   { bool c = g_cpu.flag_C; g_cpu.flag_C = v >> 7;  v = (uint8_t)((v << 1) | c);  g_nz8(v);  return v; }
static inline uint16_t g_rol16(uint16_t v) { bool c = g_cpu.flag_C; g_cpu.flag_C = v >> 15; v = (uint16_t)((v << 1) | c); g_nz16(v); return v; }
static inline uint8_t  g_ror8(uint8_t v)   { bool c = g_cpu.flag_C; g_cpu.flag_C = v & 1; v = (uint8_t)((v >> 1) | (c << 7));   g_nz8(v);  return v; }
static inline uint16_t g_ror16(uint16_t v) { bool c = g_cpu.flag_C; g_cpu.flag_C = v & 1; v = (uint16_t)((v >> 1) | (c << 15)); g_nz16(v); return v; }

/* ---- BIT / TSB / TRB ---------------------------------------------------- */
static inline void g_bit8(uint8_t m, bool imm) {
    g_cpu.flag_Z = ((uint8_t)g_cpu.C & m) == 0;
    if (!imm) { g_cpu.flag_N = (m & 0x80) != 0; g_cpu.flag_V = (m & 0x40) != 0; }
}
static inline void g_bit16(uint16_t m, bool imm) {
    g_cpu.flag_Z = (g_cpu.C & m) == 0;
    if (!imm) { g_cpu.flag_N = (m & 0x8000) != 0; g_cpu.flag_V = (m & 0x4000) != 0; }
}

/* ---- block moves -------------------------------------------------------- */
/* MVN/MVP: move C+1 bytes from src_bank:X to dst_bank:Y, DB = dst_bank. */
static inline void g_mvn(uint8_t dst, uint8_t src) {
    g_cpu.DB = dst;
    do {
        bus_write8(dst, g_cpu.Y, bus_read8(src, g_cpu.X));
        g_cpu.X = g_cpu.flag_X ? (uint8_t)(g_cpu.X + 1) : (uint16_t)(g_cpu.X + 1);
        g_cpu.Y = g_cpu.flag_X ? (uint8_t)(g_cpu.Y + 1) : (uint16_t)(g_cpu.Y + 1);
    } while (g_cpu.C-- != 0);
}
static inline void g_mvp(uint8_t dst, uint8_t src) {
    g_cpu.DB = dst;
    do {
        bus_write8(dst, g_cpu.Y, bus_read8(src, g_cpu.X));
        g_cpu.X = g_cpu.flag_X ? (uint8_t)(g_cpu.X - 1) : (uint16_t)(g_cpu.X - 1);
        g_cpu.Y = g_cpu.flag_X ? (uint8_t)(g_cpu.Y - 1) : (uint16_t)(g_cpu.Y - 1);
    } while (g_cpu.C-- != 0);
}

#endif /* SNESRECOMP_GEN_OPS_H */
