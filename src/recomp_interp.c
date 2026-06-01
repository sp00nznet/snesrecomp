/*
 * Interpreter fallback for not-yet-recompiled code.
 *
 * snesrecomp's model is "the recompiled native code IS the CPU" — the
 * LakeSnes CPU is normally never driven. That works only as far as code
 * has actually been hand-recompiled; any JSR/JSL into an un-recompiled
 * routine would otherwise be silently skipped.
 *
 * This module bridges the gap: when the dispatch table has no native
 * function for an address, we run the ORIGINAL 65816 code for that
 * subroutine on the real LakeSnes CPU, against the same hardware/WRAM the
 * recompiled code uses. Recompiled and interpreted code interoperate
 * freely because both ultimately read/write the same bus.
 *
 * Key design point: we do NOT advance SNES master-cycle timing while
 * interpreting. The normal LakeSnes CPU memory handlers (snes_cpuRead/
 * Write/Idle) call snes_runCycles(), which steps the PPU scanline renderer
 * and fires NMI/IRQ at vblank. snesrecomp drives the PPU and NMI manually
 * (see snesrecomp_end_frame / the recompiled NMI dispatch), so letting the
 * CPU advance timing here would double-render lines and fire spurious
 * interrupts mid-subroutine. Instead we temporarily swap in untimed memory
 * handlers that route through bus_read8/bus_write8 — exactly the same
 * untimed bus the recompiled functions use (including the DMA 2-step fix).
 */

#include "snesrecomp/func_table.h"
#include "snesrecomp/cpu.h"
#include "snesrecomp/bus.h"

#include "snes.h"
#include "cpu.h"
#include "apu.h"

#include <stdio.h>
#include <stdlib.h>

extern Snes *snesrecomp_get_snes(void);

static bool s_interp_enabled = true;
static bool s_interp_force   = false;

void recomp_interp_set_enabled(bool enabled) { s_interp_enabled = enabled; }
bool recomp_interp_enabled(void) { return s_interp_enabled; }

/* Force mode: dispatch always interprets the original ROM, ignoring any
 * recompiled native function registered for the address. Lets the genuine
 * game logic drive the menus/gameplay while the recomp grows incrementally.
 * The recompiled frame "shells" (boot, NMI/main alternation, brightness)
 * still run in C; only the dispatched bodies are interpreted. */
void recomp_interp_set_force(bool force) { s_interp_force = force; }
bool recomp_interp_force(void) { return s_interp_force; }

/* --- Untimed memory handlers (route through the recomp bus) --- */

/* Advance the SPC700/APU when interpreted code touches the APU I/O ports
 * ($2140-$2143). Untimed interpretation never steps the APU, so code that
 * polls these ports for an SPC handshake (audio-driver upload, per-frame
 * sound-engine sync) would spin forever. Letting the APU catch up on each
 * port access lets those handshakes converge. The APU is independent of
 * PPU/CPU timing, so advancing it here doesn't disturb the manual frame
 * model. */
static void apu_catchup_if_port(Snes *snes, uint32_t adr) {
    uint16_t a = (uint16_t)adr;
    if (a >= 0x2140 && a <= 0x2143 && snes->apu) {
        apu_runCycles(snes->apu, 64);
    }
}

/* Advance the PPU H/V scan position when interpreted code polls the H/V-blank
 * status register ($4212/HVBJOY). Untimed interpretation never steps the PPU,
 * so hPos/vPos stay frozen and a "wait for HBlank/VBlank" spin-loop would never
 * exit. We bump the position directly (NOT via snes_runCycle, which would render
 * scanlines and fire NMI) so the blank flags cycle and the wait completes.
 * $4212: bit6 HBlank = (hPos<4 || hPos>=1096), bit7 VBlank = inVblank. */
static void ppu_status_advance(Snes *snes, uint32_t adr) {
    if ((uint16_t)adr != 0x4212) return;
    snes->hPos += 192;
    if (snes->hPos >= 1364) {
        snes->hPos -= 1364;
        snes->vPos++;
        if (snes->vPos >= 262) snes->vPos = 0;
        snes->inVblank = (snes->vPos >= 225);
    }
}

static uint8_t interp_read(void *mem, uint32_t adr) {
    Snes *snes = (Snes *)mem;
    if (snes) {
        apu_catchup_if_port(snes, adr);
        ppu_status_advance(snes, adr);
    }
    return bus_read8((uint8_t)(adr >> 16), (uint16_t)adr);
}

/* Optional debug: log interpreted writes to a watched address with the PC.
 *   SMK_WATCH_REG=<hex>   — match low 16 bits, I/O-capable banks only ($00-3F/$80-BF)
 *   SMK_WATCH_ADDR=<hex>  — match the full 24-bit address exactly (e.g. WRAM 7E9F40) */
static long s_watch_reg  = -2;
static long s_watch_addr = -2;

static void interp_write(void *mem, uint32_t adr, uint8_t val) {
    Snes *snes = (Snes *)mem;
    if (s_watch_reg == -2) {
        const char *w = getenv("SMK_WATCH_REG");
        s_watch_reg = w ? strtol(w, NULL, 16) : -1;
    }
    if (s_watch_addr == -2) {
        const char *w = getenv("SMK_WATCH_ADDR");
        s_watch_addr = w ? strtol(w, NULL, 16) : -1;
    }
    if (snes && snes->cpu) {
        /* I/O register watch: $21xx etc. are registers only in banks with bit6
         * clear ($00-3F / $80-BF); in $7E/$7F the same low address is WRAM. */
        if (s_watch_reg >= 0 && (uint16_t)adr == (uint16_t)s_watch_reg &&
            (((adr >> 16) & 0x40) == 0)) {
            fprintf(stderr, "WR $%04X <- %02X  pc=%02X:%04X\n",
                    (uint16_t)adr, val, snes->cpu->k, snes->cpu->pc);
        }
        if (s_watch_addr >= 0 && (adr & 0xFFFFFF) == (uint32_t)s_watch_addr) {
            fprintf(stderr, "WR $%06X <- %02X  pc=%02X:%04X\n",
                    adr & 0xFFFFFF, val, snes->cpu->k, snes->cpu->pc);
        }
    }
    if (snes) apu_catchup_if_port(snes, adr);
    bus_write8((uint8_t)(adr >> 16), (uint16_t)adr, val);
}

static void interp_idle(void *mem, bool waiting) {
    (void)mem;
    (void)waiting;
}

/* --- Register state sync between g_cpu and the LakeSnes CPU --- */

static void sync_to_lake(Cpu *c) {
    c->a  = g_cpu.C;
    c->x  = g_cpu.X;
    c->y  = g_cpu.Y;
    c->sp = g_cpu.S;
    c->dp = g_cpu.DP;
    c->db = g_cpu.DB;
    c->k  = g_cpu.PB;
    c->c  = g_cpu.flag_C;
    c->z  = g_cpu.flag_Z;
    c->i  = g_cpu.flag_I;
    c->d  = g_cpu.flag_D;
    c->xf = g_cpu.flag_X;
    c->mf = g_cpu.flag_M;
    c->v  = g_cpu.flag_V;
    c->n  = g_cpu.flag_N;
    c->e  = g_cpu.flag_E;
}

static void sync_from_lake(Cpu *c) {
    g_cpu.C      = c->a;
    g_cpu.X      = c->x;
    g_cpu.Y      = c->y;
    g_cpu.S      = c->sp;
    g_cpu.DP     = c->dp;
    g_cpu.DB     = c->db;
    g_cpu.PB     = c->k;
    g_cpu.flag_C = c->c;
    g_cpu.flag_Z = c->z;
    g_cpu.flag_I = c->i;
    g_cpu.flag_D = c->d;
    g_cpu.flag_X = c->xf;
    g_cpu.flag_M = c->mf;
    g_cpu.flag_V = c->v;
    g_cpu.flag_N = c->n;
    g_cpu.flag_E = c->e;
}

/* Safety cap: number of opcodes to interpret before bailing out. A correct
 * per-frame handler executes well under this; a routine that spins waiting
 * for NMI (which we drive externally) would otherwise loop forever. */
#define INTERP_MAX_OPS 20000000L

void recomp_interp_call(uint32_t snes_addr, bool is_long) {
    Snes *snes = snesrecomp_get_snes();
    if (!snes || !snes->cpu) return;
    Cpu *c = snes->cpu;

    /* Save and swap in untimed memory handlers */
    void            *saved_mem   = c->mem;
    CpuReadHandler   saved_read  = c->read;
    CpuWriteHandler  saved_write = c->write;
    CpuIdleHandler   saved_idle  = c->idle;
    c->mem   = snes;
    c->read  = interp_read;
    c->write = interp_write;
    c->idle  = interp_idle;

    sync_to_lake(c);

    /* No interrupts/halt may interfere: the recomp drives NMI itself, and we
     * never advance timing here so none would fire anyway. */
    c->waiting     = false;
    c->stopped     = false;
    c->nmiWanted   = false;
    c->intWanted   = false;
    c->irqWanted   = false;
    c->resetWanted = false;

    uint16_t start_sp = c->sp;

    /* Push a sentinel return frame. The values don't matter — completion is
     * detected purely by the stack pointer climbing back to start_sp when the
     * routine's terminating RTL/RTS pulls these bytes. JSL pushes a 24-bit
     * return (bank + 16-bit PC); JSR pushes a 16-bit return. */
    if (is_long) {
        bus_write8(0x00, c->sp, 0x00);  /* return bank */
        c->sp--;
    }
    bus_write8(0x00, c->sp, 0xFF);       /* return PC high */
    c->sp--;
    bus_write8(0x00, c->sp, 0xFF);       /* return PC low */
    c->sp--;

    c->k  = (snes_addr >> 16) & 0xFF;
    c->pc = (uint16_t)snes_addr;

    /* Optional control-flow trace: record (k:pc) before each opcode in a ring
     * buffer and dump the tail on abort, to locate runaway loops. */
    static int trace_on = -1;
    if (trace_on < 0) trace_on = getenv("SMK_INTERP_TRACE") ? 1 : 0;
    enum { TRC = 64 };
    static uint32_t trc[TRC];
    int trci = 0;

    /* Optional exec trace: when PC reaches SMK_TRACE_EXEC=<24-bit hex>, log the
     * caller's return address off the stack (works for routines reached via
     * indirect dispatch that static xref can't find). */
    static long trace_exec = -2;
    if (trace_exec == -2) {
        const char *te = getenv("SMK_TRACE_EXEC");
        trace_exec = te ? strtol(te, NULL, 16) : -1;
    }

    long ops = 0;
    bool aborted = false;
    /* Run until the stack unwinds back to (or past) the entry level. Using a
     * signed difference handles the exact-return case (== 0) and any overshoot
     * (< 0) without a separate check. */
    while ((int16_t)(start_sp - c->sp) > 0) {
        if (trace_on) { trc[trci % TRC] = ((uint32_t)c->k << 16) | c->pc; trci++; }
        if (trace_exec >= 0 && (((uint32_t)c->k << 16) | c->pc) == (uint32_t)trace_exec) {
            uint16_t sp = c->sp;
            uint16_t ret = bus_read8(0x00, (sp + 1) & 0xFFFF) |
                           (bus_read8(0x00, (sp + 2) & 0xFFFF) << 8);
            fprintf(stderr, "TRACE_EXEC $%06lX hit: sp=%04X ret=%04X (caller~%04X) entry-routine=$%06X\n",
                    (long)trace_exec, sp, ret, (uint16_t)(ret - 2), snes_addr);
        }
        cpu_runOpcode(c);
        if (c->waiting || c->stopped) {
            fprintf(stderr, "recomp_interp: $%06X executed WAI/STP — aborting "
                            "(this routine likely waits on NMI)\n", snes_addr);
            aborted = true;
            break;
        }
        if (++ops > INTERP_MAX_OPS) {
            fprintf(stderr, "recomp_interp: $%06X exceeded %ld-op cap "
                            "(sp=%04X start=%04X pc=%02X:%04X) — aborting\n",
                    snes_addr, (long)INTERP_MAX_OPS, c->sp, start_sp, c->k, c->pc);
            aborted = true;
            if (trace_on) {
                fprintf(stderr, "  trace (last %d opcodes, oldest first):\n", TRC);
                int start = (trci > TRC) ? (trci - TRC) : 0;
                for (int i = start; i < trci; i++) {
                    fprintf(stderr, " %06X", trc[i % TRC]);
                    if ((i - start) % 12 == 11) fprintf(stderr, "\n");
                }
                fprintf(stderr, "\n");
            }
            break;
        }
    }

    /* On abort, the routine left an unbalanced stack. Restore the entry stack
     * pointer so the bailout doesn't leak frames into subsequent calls (which
     * would otherwise drift the stack down until it corrupts WRAM/overflows). */
    if (aborted) c->sp = start_sp;

    sync_from_lake(c);

    /* Restore the original (timed) memory handlers */
    c->mem   = saved_mem;
    c->read  = saved_read;
    c->write = saved_write;
    c->idle  = saved_idle;
}
