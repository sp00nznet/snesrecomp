#ifndef SNESRECOMP_BUS_H
#define SNESRECOMP_BUS_H

/*
 * Memory bus — the bridge between your recompiled code and real SNES hardware.
 *
 * When your recompiled function does:
 *   STA $2118    (write to PPU VRAM data port)
 *
 * It becomes:
 *   bus_write8(g_cpu.DB, 0x2118, CPU_A8());
 *
 * That write goes through LakeSnes's full memory bus to the real PPU,
 * which updates VRAM exactly like the original hardware would.
 *
 * Same for reads — LDA $4210 (read NMI status) becomes:
 *   CPU_SET_A8(bus_read8(0x00, 0x4210));
 *
 * You get cycle-accurate hardware behavior for free.
 */

#include <stdint.h>
#include <stdbool.h>

/* 24-bit address space reads/writes (bank:addr) */
uint8_t  bus_read8(uint8_t bank, uint16_t addr);
void     bus_write8(uint8_t bank, uint16_t addr, uint8_t val);
uint16_t bus_read16(uint8_t bank, uint16_t addr);
void     bus_write16(uint8_t bank, uint16_t addr, uint16_t val);

/* Direct WRAM access (faster than bus routing for stack/DP operations) */
uint8_t  bus_wram_read8(uint32_t offset);
void     bus_wram_write8(uint32_t offset, uint8_t val);
uint16_t bus_wram_read16(uint32_t offset);
void     bus_wram_write16(uint32_t offset, uint16_t val);

/* Get pointer to WRAM (128KB) for bulk access */
uint8_t *bus_get_wram(void);

/* Get pointer to ROM data */
const uint8_t *bus_get_rom(uint32_t *size_out);

/*
 * Direct SPC700 RAM access — for fast bulk uploads that bypass the
 * polled transfer protocol. Use this instead of the byte-by-byte
 * echo handshake when uploading large blocks to the audio CPU.
 */
void     bus_apu_write_ram(uint16_t spc_addr, uint8_t val);
uint8_t  bus_apu_read_ram(uint16_t spc_addr);

/*
 * Advance SNES master cycles — runs the SPC700 (and timers) forward.
 *
 * Recompiled code bypasses the normal CPU cycle counting, so polling loops
 * that wait on APU port responses (e.g., the SPC700 IPL upload protocol)
 * will deadlock unless you explicitly advance time.
 *
 * Call this inside tight polling loops that read APU ports ($2140-$2143).
 * A reasonable value is 8-64 master cycles per iteration (one 65816
 * instruction is typically 6-8 master cycles).
 */
void bus_run_cycles(int master_cycles);

/*
 * Super FX / GSU access — for recompiled code that needs to interact
 * with the GSU coprocessor directly (e.g., writing R15 to start execution,
 * reading status flags, setting up screen parameters).
 *
 * These are thin wrappers around gsu_read/gsu_write.
 * Returns 0 / no-op if no GSU is present.
 */
uint8_t bus_gsu_read(uint16_t addr);
void    bus_gsu_write(uint16_t addr, uint8_t val);

/* Trigger a GSU execution run (call after writing R15 high byte) */
void    bus_gsu_run(void);

/* Check if cartridge has a GSU */
bool    bus_has_gsu(void);

#endif /* SNESRECOMP_BUS_H */
