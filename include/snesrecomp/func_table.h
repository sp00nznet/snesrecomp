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
snes_func_t func_table_lookup(uint32_t snes_addr);

/*
 * Dispatch to a routine by SNES address.
 *
 * If a recompiled native function is registered for the address, it is
 * called. Otherwise, if the interpreter fallback is enabled, the original
 * 65816 code at that address is executed on the LakeSnes CPU (see
 * recomp_interp_call) and the call still "succeeds" (returns true). Only
 * when interpretation is disabled does an unregistered address return false.
 *
 * func_table_call     — JSL/RTL return semantics (long subroutine call).
 * func_table_call_jsr — JSR/RTS return semantics (in-bank subroutine call).
 *
 * The RTS/RTL distinction matters only for the interpreter fallback; a
 * registered native function returns via normal C and ignores it.
 */
bool func_table_call(uint32_t snes_addr);
bool func_table_call_jsr(uint32_t snes_addr);

/*
 * Interpreter fallback: run original ROM code on the LakeSnes CPU for a
 * subroutine that has not been recompiled. is_long selects RTL (JSL) vs
 * RTS (JSR) return semantics. Uses untimed bus access (no PPU/NMI timing
 * side effects) so it composes with snesrecomp's manual frame model.
 */
void recomp_interp_call(uint32_t snes_addr, bool is_long);
void recomp_interp_set_enabled(bool enabled);
bool recomp_interp_enabled(void);

/*
 * Force mode: when enabled, func_table_call / func_table_call_jsr always
 * interpret the original ROM at the address, bypassing any recompiled native
 * function. Useful to run the genuine game end-to-end while recompiled
 * functions are still partial. Recompiled frame "shells" that call C
 * functions directly are unaffected.
 */
void recomp_interp_set_force(bool force);
bool recomp_interp_force(void);

/*
 * Timed-recomp interception (Phase-1 of replacing real-frame mode). In the
 * timed frame model (snes_runFrame drives real PPU/APU/NMI timing), enabling
 * this installs a CPU opcode-fetch hook: whenever the timed CPU reaches a
 * registered entry address, the recompiled native function runs in place of
 * the ROM subroutine and PB:PC is advanced past it (simulated RTS/RTL). Code
 * that is NOT registered keeps running on the timed CPU (full hardware
 * fidelity). Game-agnostic: the host chooses which entries to intercept.
 *
 * add_intercept: register one entry (is_long = JSL/RTL vs JSR/RTS return).
 * enable/disable: install/remove the hook.
 * intercept_hits: count of interceptions so far (diagnostics).
 */
void          recomp_timed_add_intercept(uint32_t snes_addr, bool is_long);
void          recomp_timed_recomp_enable(void);
void          recomp_timed_recomp_disable(void);
unsigned long recomp_timed_intercept_hits(void);

/*
 * For native code that polls a hardware flag the ROM spins on ($4212 auto-read
 * busy, HVBJOY, an APU port). Inside the timed loop it advances the clock by
 * master_cycles (about one poll iteration) and returns true, so the flag can
 * change; elsewhere it returns false, meaning time will not pass and the caller
 * should stop spinning. Translate  `-: LDA $4212 : AND #1 : BNE -`  as
 *     while ((bus_read8(0x00, 0x4212) & 1) && recomp_timed_spin(64)) {}
 */
bool recomp_timed_spin(int master_cycles);

/*
 * Lockstep validation: on each call of snes_addr inside the timed loop, run the
 * registered native body on a saved copy of the machine, restore, let the ROM
 * routine run, and compare WRAM/VRAM/CGRAM/OAM when it returns. Execution stays
 * genuine, so a mismatch is the function's own fault, not a timing knock-on.
 * Needs recomp_timed_recomp_enable(). recomp_timed_validate_report() prints one
 * "VALIDATE addr calls= mem_fail= reg_fail= skipped=" line per function.
 */
void recomp_timed_add_validate(uint32_t snes_addr, bool is_long);
void recomp_timed_validate_report(void);

/* Call-target profiler: rank the hottest direct JSR/JSL targets (best
 * recompilation candidates). Enable before the run; dump the top-N at the end. */
void recomp_timed_profile_enable(void);
void recomp_timed_profile_dump(int top);

/* For an intercepted recompiled function whose original ends in JMP/JML rather
 * than RTS/RTL: call this from the body instead of returning normally, and the
 * timed-recomp hook will transfer control to snes_addr (no stack pop). */
void recomp_set_redirect(uint32_t snes_addr);

#endif /* SNESRECOMP_FUNC_TABLE_H */
