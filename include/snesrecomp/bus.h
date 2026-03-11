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

#endif /* SNESRECOMP_BUS_H */
