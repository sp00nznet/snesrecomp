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
#include <string.h>

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

static unsigned long s_interp_calls = 0;   /* lets validation see a fallback */

void recomp_interp_call(uint32_t snes_addr, bool is_long) {
    s_interp_calls++;
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
    /* DMA-register guard across interrupts taken DURING an interpreted routine.
     *
     * snesrecomp doesn't advance PPU timing in interp, but interpreted writes to
     * $4200/$4207-$420A can still latch irqWanted, so an IRQ/NMI may be serviced
     * mid-routine. The handler can re-enter game code that programs DMA channel 0
     * (e.g. SMK's OBJ-palette CGRAM upload) — which on real hardware is harmless
     * because the interrupt fires at a different cycle, but here lands in the
     * MIDDLE of another routine's DMA-flush loop and leaves the channel's B-bus
     * address pointing at CGRAM. The resumed flush then DMAs VRAM tile data into
     * CGRAM, corrupting the Mode-7 race palette.
     *
     * Rather than suppress the interrupt (which would drop its legitimate work),
     * snapshot the DMA channel state when an interrupt is taken and restore it
     * once the handler's RTI unwinds the stack back. The interrupt's own DMAs
     * already executed (writing CGRAM/VRAM) before the restore, so they stick;
     * only the channel *registers* are rewound so the interrupted loop resumes
     * with the setup it had. */
    DmaChannel saved_dma[8];
    bool dma_guard_pending = false;
    uint16_t dma_guard_sp = 0;

    while ((int16_t)(start_sp - c->sp) > 0) {
        if (c->intWanted && !dma_guard_pending) {
            memcpy(saved_dma, snes->dma->channel, sizeof(saved_dma));
            dma_guard_sp = c->sp;       /* RTI will return SP to here */
            dma_guard_pending = true;
        }
        if (trace_on) { trc[trci % TRC] = ((uint32_t)c->k << 16) | c->pc; trci++; }
        if (trace_exec >= 0 && (((uint32_t)c->k << 16) | c->pc) == (uint32_t)trace_exec) {
            uint16_t sp = c->sp;
            uint16_t ret = bus_read8(0x00, (sp + 1) & 0xFFFF) |
                           (bus_read8(0x00, (sp + 2) & 0xFFFF) << 8);
            fprintf(stderr, "TRACE_EXEC $%06lX hit: sp=%04X ret=%04X (caller~%04X) entry-routine=$%06X\n",
                    (long)trace_exec, sp, ret, (uint16_t)(ret - 2), snes_addr);
        }
        cpu_runOpcode(c);
        /* Once the interrupt handler's RTI has unwound the stack back to (or
         * above) the pre-interrupt level, restore the DMA channel registers the
         * interrupted routine had programmed. */
        if (dma_guard_pending && (int16_t)(c->sp - dma_guard_sp) >= 0) {
            memcpy(snes->dma->channel, saved_dma, sizeof(saved_dma));
            dma_guard_pending = false;
        }
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

    /* SNESRECOMP_INTERP_STATS=1 — one line per interpreted routine: how many
     * opcodes it actually ran and where it stopped. A routine that "returns"
     * after a handful of ops has almost certainly tripped the stack-unwind
     * completion check early rather than genuinely finishing. */
    if (getenv("SNESRECOMP_INTERP_STATS"))
        fprintf(stderr, "[INTERP] $%06X ran %ld ops, end pc=%02X:%04X sp=%04X (start_sp=%04X)%s\n",
                snes_addr, ops, c->k, c->pc, c->sp, start_sp, aborted ? " ABORTED" : "");

    sync_from_lake(c);

    /* Restore the original (timed) memory handlers */
    c->mem   = saved_mem;
    c->read  = saved_read;
    c->write = saved_write;
    c->idle  = saved_idle;
}

/* ===== Phase-1 timed-recomp interception ==================================
 *
 * Unlike recomp_interp_call (above), which runs ROM on the LakeSnes CPU with
 * UNTIMED handlers for the old manual-frame model, this path runs inside the
 * TIMED frame loop (snes_runFrame, real PPU/APU/NMI timing — same as
 * real-frame mode). The CPU calls g_cpuRecompHook at every opcode fetch; when
 * PB:PC matches a registered entry we run the recompiled native body in place
 * of the ROM subroutine and advance PB:PC past it via a simulated RTS/RTL.
 *
 * Cycles "skipped" by running the native body instantly are reabsorbed by the
 * genuine code's next vblank spin-wait (which keeps stepping the timed CPU
 * until the PPU actually reaches vblank), so ordinary (non-raster-timed)
 * routines need no per-function cycle accounting. */

extern bool (*g_cpuRecompHook)(Cpu*);

typedef struct { uint32_t addr; bool is_long; } TimedIntercept;
static TimedIntercept s_intercepts[128];
static int            s_intercept_count = 0;
static unsigned long  s_intercept_hits  = 0;

void recomp_timed_add_intercept(uint32_t snes_addr, bool is_long) {
    for (int i = 0; i < s_intercept_count; i++)
        if (s_intercepts[i].addr == snes_addr) return;   /* dedupe */
    if (s_intercept_count < (int)(sizeof(s_intercepts) / sizeof(s_intercepts[0]))) {
        s_intercepts[s_intercept_count].addr    = snes_addr;
        s_intercepts[s_intercept_count].is_long = is_long;
        s_intercept_count++;
    }
}

unsigned long recomp_timed_intercept_hits(void) { return s_intercept_hits; }

/* Depth of native bodies currently running inside the timed loop. */
static int s_timed_depth = 0;

bool recomp_timed_spin(int master_cycles) {
    Snes *sn = snesrecomp_get_snes();
    if (!s_timed_depth || !sn) return false;
    snes_runCycles(sn, master_cycles);
    return true;
}

/* Redirect protocol: an intercepted function whose original ends in a JMP/JML
 * (not RTS/RTL) calls recomp_set_redirect(target) instead of returning; the
 * hook then sets PB:PC to the target (no stack pop), matching the jump. */
static uint32_t s_redirect = 0xFFFFFFFFu;   /* sentinel = no redirect */
void recomp_set_redirect(uint32_t snes_addr) { s_redirect = snes_addr; }

/* --- Call-target profiler (Phase-3 target finding) -----------------------
 * When enabled, tallies the target of every direct JSR/JSL the timed CPU
 * executes, so the hottest leaves (best recompilation targets) can be ranked.
 * Indirect dispatch (JSR ($table,x)) is not target-resolvable here and is
 * skipped — leaves are virtually always called directly. */
/* Records each direct JSR/JSL target with the M/X flags the callee enters with
 * (= the caller's current flags; JSR/JSL preserve P), and a multi-flag marker so
 * the auto-generator can skip per-(M,X) variant targets. */
typedef struct { uint32_t addr; unsigned long count; uint8_t m, x, multi, jsl; } CallTally;
static CallTally s_tally[8192];
static int       s_tally_n  = 0;
static bool      s_profile  = false;

void recomp_timed_profile_enable(void) { s_profile = true; }

static void tally_call(uint32_t target, uint8_t m, uint8_t x, uint8_t jsl) {
    for (int i = 0; i < s_tally_n; i++)
        if (s_tally[i].addr == target) {
            s_tally[i].count++;
            if (s_tally[i].m != m || s_tally[i].x != x) s_tally[i].multi = 1;
            if (i > 0) {  /* move-to-front: keep hot targets cheap to find */
                CallTally t = s_tally[i]; s_tally[i] = s_tally[i - 1]; s_tally[i - 1] = t;
            }
            return;
        }
    if (s_tally_n < (int)(sizeof(s_tally) / sizeof(s_tally[0]))) {
        s_tally[s_tally_n].addr = target;
        s_tally[s_tally_n].count = 1;
        s_tally[s_tally_n].m = m; s_tally[s_tally_n].x = x; s_tally[s_tally_n].multi = 0;
        s_tally[s_tally_n].jsl = jsl;
        s_tally_n++;
    }
}

void recomp_timed_profile_dump(int top) {
    fprintf(stderr, "=== timed-recomp call profile: %d distinct JSR/JSL targets ===\n", s_tally_n);
    fprintf(stderr, "# addr   count    P(entry) call flags\n");
    for (int k = 0; k < top; k++) {
        int best = -1; unsigned long bc = 0;
        for (int i = 0; i < s_tally_n; i++)
            if (s_tally[i].count > bc) { bc = s_tally[i].count; best = i; }
        if (best < 0) break;
        CallTally *t = &s_tally[best];
        uint8_t P = (uint8_t)((t->m ? 0x20 : 0) | (t->x ? 0x10 : 0));
        fprintf(stderr, "PROF %06X %-8lu %02X %s %s%s\n", t->addr, t->count, P, t->jsl ? "JSL" : "JSR",
                t->multi ? "MULTI-MX " : "",
                func_table_lookup(t->addr) ? "recompiled" : "");
        t->count = 0;  /* consume so the next pass finds the next-highest */
    }
}

/* --- Lockstep validation ---------------------------------------------------
 *
 * Checks a recompiled function against the genuine routine on every call,
 * without letting it change the run. At each entry: save the machine, run the
 * native body, keep what it produced, restore, and let the ROM routine run
 * for real. When the ROM routine returns (PB:PC = return address, S back to
 * its pre-call value), compare WRAM, VRAM, CGRAM and OAM with what the native
 * body produced.
 *
 * Intercepting instead (recomp_timed_add_intercept) is not a usable test: the
 * native body costs zero cycles, so later work lands on different frames and a
 * correct function still produces a different snapshot. Here execution stays
 * genuine, so any difference is the function's own.
 *
 * The last value written to each PPU/CPU I/O register ($2100-$21FF,
 * $4200-$43FF) is compared too, since a register write (INIDISP brightness,
 * a screen enable) leaves no trace in memory. Data ports ($2104, $2118/9,
 * $2122, $2180) and the joypad strobe $4016 are left out: their effect is the
 * memory they fill, which is compared anyway.
 *
 * Bytes just below the entry stack pointer are not compared: the ROM routine
 * pushes there and a native body does not. Registers are compared but reported
 * separately (a hand-port often leaves A/X/Y different where no caller looks).
 * A native body that exits through recomp_set_redirect is counted as skipped.
 */
typedef struct {
    uint32_t addr; bool is_long;
    unsigned long calls, mem_fail, reg_fail, skipped, mx_fail;
    char first[200];
    char first_reg[160];
} Validate;
static Validate s_val[256];
static int      s_val_n = 0;

typedef struct {
    int idx; uint8_t k; uint16_t pc, sp, entry_sp;
    uint8_t *ram; uint16_t *vram, *cgram, *oam; uint8_t *hoam;
    uint16_t a, x, y, dp; uint8_t db, c, z, n, v, mf, xf;
    uint8_t nio[0x300], nio_set[0x300];   /* native body's register writes */
    uint8_t gio[0x300], gio_set[0x300];   /* genuine routine's */
} Pending;
#define VAL_DEPTH 8
static Pending s_pend[VAL_DEPTH];
static int     s_pend_n = 0;
static uint8_t *s_state_buf = NULL;
static Pending *s_native_cap = NULL;          /* set while a native body runs */
static CpuWriteHandler s_genuine_write = NULL; /* CPU write handler we wrap */

static int io_index(uint32_t adr) {
    uint8_t bank = (uint8_t)(adr >> 16);
    uint16_t a = (uint16_t)adr;
    if (bank >= 0x40 && bank < 0x80) return -1;
    if (bank >= 0xC0) return -1;
    if (a == 0x2104 || a == 0x2118 || a == 0x2119 || a == 0x2122 || a == 0x2180) return -1;
    if (a >= 0x2100 && a <= 0x21FF) return a - 0x2100;
    if (a >= 0x4200 && a <= 0x43FF) return 0x100 + (a - 0x4200);
    return -1;
}

void recomp_validate_note_write(uint32_t adr, uint8_t val) {
    int i;
    if (!s_native_cap || (i = io_index(adr)) < 0) return;
    s_native_cap->nio[i] = val;
    s_native_cap->nio_set[i] = 1;
}

static void genuine_write(void *mem, uint32_t adr, uint8_t val) {
    int i = io_index(adr);
    if (i >= 0)
        for (int k = 0; k < s_pend_n; k++) { s_pend[k].gio[i] = val; s_pend[k].gio_set[i] = 1; }
    s_genuine_write(mem, adr, val);
}
extern bool g_recomp_validating, g_recomp_frame_touched;

void recomp_timed_add_validate(uint32_t snes_addr, bool is_long) {
    for (int i = 0; i < s_val_n; i++)
        if (s_val[i].addr == snes_addr) return;
    if (s_val_n < (int)(sizeof(s_val) / sizeof(s_val[0]))) {
        memset(&s_val[s_val_n], 0, sizeof(Validate));
        s_val[s_val_n].addr = snes_addr;
        s_val[s_val_n].is_long = is_long;
        s_val_n++;
    }
}

void recomp_timed_validate_report(void) {
    for (int i = 0; i < s_val_n; i++) {
        Validate *v = &s_val[i];
        fprintf(stderr, "VALIDATE %06X calls=%lu mem_fail=%lu reg_fail=%lu mx_fail=%lu skipped=%lu%s%s%s%s\n",
                v->addr, v->calls, v->mem_fail, v->reg_fail, v->mx_fail, v->skipped,
                v->first[0] ? " first: " : "", v->first,
                v->first_reg[0] ? " | reg: " : "", v->first_reg);
    }
}

static void validate_compare(Cpu *c, Pending *p) {
    Snes *sn = snesrecomp_get_snes();
    Validate *v = &s_val[p->idx];
    char buf[200] = "";
    int n = 0;
    for (int i = 0; i < 0x20000; i++) {
        if (i <= p->entry_sp && i > p->entry_sp - 0x100) continue;   /* stack scratch */
        if (sn->ram[i] != p->ram[i] && n++ == 0)
            snprintf(buf, sizeof(buf), "wram $%05X rom=%02X native=%02X", i, sn->ram[i], p->ram[i]);
    }
    for (int i = 0; i < 0x8000; i++)
        if (sn->ppu->vram[i] != p->vram[i] && n++ == 0)
            snprintf(buf, sizeof(buf), "vram word $%04X rom=%04X native=%04X", i, sn->ppu->vram[i], p->vram[i]);
    for (int i = 0; i < 0x100; i++)
        if (sn->ppu->cgram[i] != p->cgram[i] && n++ == 0)
            snprintf(buf, sizeof(buf), "cgram $%02X rom=%04X native=%04X", i, sn->ppu->cgram[i], p->cgram[i]);
    for (int i = 0; i < 0x100; i++)
        if (sn->ppu->oam[i] != p->oam[i] && n++ == 0)
            snprintf(buf, sizeof(buf), "oam word $%02X rom=%04X native=%04X", i, sn->ppu->oam[i], p->oam[i]);
    for (int i = 0; i < 0x20; i++)
        if (sn->ppu->highOam[i] != p->hoam[i] && n++ == 0)
            snprintf(buf, sizeof(buf), "high oam $%02X rom=%02X native=%02X", i, sn->ppu->highOam[i], p->hoam[i]);
    for (int i = 0; i < 0x300; i++) {
        if (p->nio_set[i] == p->gio_set[i] && (!p->nio_set[i] || p->nio[i] == p->gio[i])) continue;
        unsigned reg = i < 0x100 ? 0x2100 + i : 0x4200 + (i - 0x100);
        if (n++ == 0)
            snprintf(buf, sizeof(buf), "io $%04X rom=%s%02X native=%s%02X", reg,
                     p->gio_set[i] ? "" : "unwritten/", p->gio[i],
                     p->nio_set[i] ? "" : "unwritten/", p->nio[i]);
    }
    v->calls++;
    if (n) {
        v->mem_fail++;
        if (!v->first[0])
            snprintf(v->first, sizeof(v->first), "call %lu, frame %u: %s (%d differ)", v->calls, sn->frames, buf, n);
    } else if (c->a != p->a || c->x != p->x || c->y != p->y || c->db != p->db || c->dp != p->dp ||
               c->c != p->c || c->z != p->z || c->n != p->n || c->v != p->v ||
               c->mf != p->mf || c->xf != p->xf) {
        v->reg_fail++;
        if (c->mf != p->mf || c->xf != p->xf) v->mx_fail++;   /* caller runs at the wrong width */
        if (!v->first_reg[0])
            snprintf(v->first_reg, sizeof(v->first_reg),
                     "rom A=%04X X=%04X Y=%04X C%d Z%d N%d V%d M%d X%d / native A=%04X X=%04X Y=%04X C%d Z%d N%d V%d M%d X%d",
                     c->a, c->x, c->y, c->c, c->z, c->n, c->v, c->mf, c->xf,
                     p->a, p->x, p->y, p->c, p->z, p->n, p->v, p->mf, p->xf);
    }
}

/* Called from the timed hook at every opcode. True = pc was a validated entry
 * (the genuine routine now runs; the hook must not intercept it). */
static bool validate_hook(Cpu *c) {
    /* A native body under check can fall back to the interpreter, which comes
     * back through this hook. Nothing it runs is genuine, and a nested check
     * would overwrite the saved state the outer one restores from. */
    if (g_recomp_validating) return false;
    uint32_t pc = ((uint32_t)c->k << 16) | (uint16_t)c->pc;

    /* A pending routine returned? Innermost first. If the stack has unwound
     * past its return frame without coming back through it (it popped its own
     * return address, as jump-table dispatchers do), there is no comparison
     * point: drop it as skipped. */
    while (s_pend_n > 0) {
        Pending *p = &s_pend[s_pend_n - 1];
        if (c->k == p->k && c->pc == p->pc && c->sp == p->sp) validate_compare(c, p);
        else if (c->sp > p->sp) s_val[p->idx].skipped++;
        else break;
        s_pend_n--;
        if (s_pend_n == 0 && c->write == genuine_write) c->write = s_genuine_write;
    }

    int idx = -1;
    for (int i = 0; i < s_val_n; i++)
        if (s_val[i].addr == pc) { idx = i; break; }
    if (idx < 0 || s_pend_n >= VAL_DEPTH) return false;
    /* Each check copies the whole machine twice, so a routine called 100k
     * times would take minutes. SNESRECOMP_VALIDATE_MAX (default 300) caps the
     * checked calls per function; later calls just run genuine. */
    static long s_max = -1;
    if (s_max < 0) { const char *e = getenv("SNESRECOMP_VALIDATE_MAX"); s_max = e ? atol(e) : 300; }
    if ((long)(s_val[idx].calls + s_val[idx].skipped) >= s_max) return false;
    snes_func_t fn = func_table_lookup(pc);
    if (!fn) return false;

    Snes *sn = snesrecomp_get_snes();
    Validate *v = &s_val[idx];
    Pending *p = &s_pend[s_pend_n];
    if (!s_state_buf) s_state_buf = (uint8_t *)malloc(1 << 20);
    if (!p->ram) {
        p->ram = (uint8_t *)malloc(0x20000);
        p->vram = (uint16_t *)malloc(0x10000);
        p->cgram = (uint16_t *)malloc(0x200);
        p->oam = (uint16_t *)malloc(0x200);
        p->hoam = (uint8_t *)malloc(0x20);
    }

    /* Where the genuine routine will come back to. */
    uint16_t sp = c->sp;
    uint16_t ret = (uint16_t)(bus_read8(0x00, (sp + 1) & 0xFFFF) | (bus_read8(0x00, (sp + 2) & 0xFFFF) << 8));
    p->idx = idx;
    p->entry_sp = sp;
    p->pc = (uint16_t)(ret + 1);
    p->k  = v->is_long ? bus_read8(0x00, (sp + 3) & 0xFFFF) : c->k;
    p->sp = (uint16_t)(sp + (v->is_long ? 3 : 2));

    int size = snes_saveState(sn, s_state_buf);
    memset(p->nio_set, 0, sizeof(p->nio_set));
    memset(p->gio_set, 0, sizeof(p->gio_set));
    s_native_cap = p;
    unsigned long interp_before = s_interp_calls;
    sync_from_lake(c);
    s_redirect = 0xFFFFFFFFu;
    s_timed_depth++;
    g_recomp_validating = true;
    g_recomp_frame_touched = false;
    fn();
    g_recomp_validating = false;
    s_native_cap = NULL;
    s_timed_depth--;
    sync_to_lake(c);
    /* Exits by jump, drives frames itself, or falls back to the (untimed)
     * interpreter for a callee: not something that can be verified as a native
     * body inside the timed loop, so the call is skipped. */
    bool redirected = s_redirect != 0xFFFFFFFFu || g_recomp_frame_touched ||
                      s_interp_calls != interp_before;
    s_redirect = 0xFFFFFFFFu;
    memcpy(p->ram, sn->ram, 0x20000);
    memcpy(p->vram, sn->ppu->vram, 0x10000);
    memcpy(p->cgram, sn->ppu->cgram, 0x200);
    memcpy(p->oam, sn->ppu->oam, 0x200);
    memcpy(p->hoam, sn->ppu->highOam, 0x20);
    p->a = c->a; p->x = c->x; p->y = c->y; p->dp = c->dp; p->db = c->db;
    p->c = c->c; p->z = c->z; p->n = c->n; p->v = c->v; p->mf = c->mf; p->xf = c->xf;
    if (!snes_loadState(sn, s_state_buf, size)) {
        fprintf(stderr, "validate: state restore failed at $%06X (size %d)\n", pc, size);
        abort();
    }

    if (redirected) { v->skipped++; return true; }
    s_pend_n++;
    if (c->write != genuine_write) { s_genuine_write = c->write; c->write = genuine_write; }
    return true;
}

static bool timed_recomp_hook(Cpu *c) {
    if (s_profile) {
        uint8_t op = bus_read8(c->k, c->pc);
        if (op == 0x20) {  /* JSR abs (near) */
            uint32_t t = ((uint32_t)c->k << 16)
                       | bus_read8(c->k, c->pc + 1)
                       | ((uint32_t)bus_read8(c->k, c->pc + 2) << 8);
            tally_call(t, c->mf, c->xf, 0);
        } else if (op == 0x22) {  /* JSL long */
            uint32_t t = bus_read8(c->k, c->pc + 1)
                       | ((uint32_t)bus_read8(c->k, c->pc + 2) << 8)
                       | ((uint32_t)bus_read8(c->k, c->pc + 3) << 16);
            tally_call(t, c->mf, c->xf, 1);
        }
    }

    if (s_val_n && validate_hook(c)) return false;   /* genuine routine runs */

    uint32_t pc = ((uint32_t)c->k << 16) | (uint16_t)c->pc;
    const TimedIntercept *e = NULL;
    for (int i = 0; i < s_intercept_count; i++)
        if (s_intercepts[i].addr == pc) { e = &s_intercepts[i]; break; }
    if (!e) return false;
    snes_func_t fn = func_table_lookup(pc);
    if (!fn) return false;

    if (getenv("SMK_RECOMP_DEBUG")) {
        Snes *sn = snesrecomp_get_snes();
        uint16_t sp = c->sp;
        uint16_t ret = bus_read8(0x00, (sp + 1) & 0xFFFF) | (bus_read8(0x00, (sp + 2) & 0xFFFF) << 8);
        fprintf(stderr, "[RECOMP] hit $%06X frame=%u db=%02X sp=%04X ret=%04X\n",
                pc, sn ? sn->frames : 0, c->db, sp, ret);
    }

    sync_from_lake(c);   /* LakeSnes CPU regs -> g_cpu (native body reads these) */
    s_redirect = 0xFFFFFFFFu;
    s_timed_depth++;
    fn();                /* run recompiled native body (g_cpu + recomp bus) */
    s_timed_depth--;
    sync_to_lake(c);     /* g_cpu -> LakeSnes CPU regs */

    /* JMP/JML exit: the function redirected instead of returning. Set PB:PC to
     * the target without popping the stack (matches the original jump). */
    if (s_redirect != 0xFFFFFFFFu) {
        c->pc = (uint16_t)s_redirect;
        c->k  = (uint8_t)(s_redirect >> 16);
        s_redirect = 0xFFFFFFFFu;
        s_intercept_hits++;
        return true;
    }

    /* Optional cycle accounting: advance the timed clock by the original
     * routine's non-DMA instruction cycles, which running the native body
     * instantly skips. SMK_RECOMP_CYCLES=N (diagnostic/tuning while the
     * per-function model is calibrated). */
    {
        static long extra = -2;
        if (extra == -2) { const char *e = getenv("SMK_RECOMP_CYCLES"); extra = e ? atol(e) : 0; }
        if (extra > 0) { Snes *sn = snesrecomp_get_snes(); if (sn) snes_runCycles(sn, (int)extra); }
    }

    /* Simulate the routine's terminating RTS (near) / RTL (long), using the
     * return frame the calling JSR/JSL pushed. Stack lives in bank $00. */
    uint16_t sp = c->sp;
    uint16_t lo = bus_read8(0x00, (sp + 1) & 0xFFFF);
    uint16_t hi = bus_read8(0x00, (sp + 2) & 0xFFFF);
    c->pc = (uint16_t)((lo | (hi << 8)) + 1);
    if (e->is_long) {
        c->k  = (uint8_t)bus_read8(0x00, (sp + 3) & 0xFFFF);
        c->sp = (uint16_t)((sp + 3) & 0xFFFF);
    } else {
        c->sp = (uint16_t)((sp + 2) & 0xFFFF);
    }
    s_intercept_hits++;
    return true;
}

void recomp_timed_recomp_enable(void)  { g_cpuRecompHook = timed_recomp_hook; }
void recomp_timed_recomp_disable(void) { g_cpuRecompHook = NULL; }
