/*
 * Memory bus adapter — routes recompiled code memory accesses to
 * LakeSnes's real SNES hardware (PPU, APU, DMA, cart, WRAM).
 *
 * Your recompiled code calls bus_read8/bus_write8 with a 24-bit
 * SNES address (bank:addr). We convert that to a flat 24-bit address
 * and hand it to LakeSnes's snes_read/snes_write, which routes it
 * to the correct hardware component.
 */

#include "snesrecomp/bus.h"
#include "snes.h"
#include "cart.h"
#include "gsu.h"

#include <string.h>
#include <stdbool.h>

/* Access to the LakeSnes instance (owned by snesrecomp.c) */
extern Snes *snesrecomp_get_snes(void);


uint8_t bus_read8(uint8_t bank, uint16_t addr) {
    Snes *snes = snesrecomp_get_snes();
    if (!snes) return 0;
    uint32_t flat = ((uint32_t)bank << 16) | addr;
    return snes_read(snes, flat);
}

void bus_write8(uint8_t bank, uint16_t addr, uint8_t val) {
    Snes *snes = snesrecomp_get_snes();
    if (!snes) return;
    uint32_t flat = ((uint32_t)bank << 16) | addr;
    snes_write(snes, flat, val);

    /* DMA fix: LakeSnes uses a 2-step state machine for DMA:
     *   Step 1: dmaState 1→2 (arm)
     *   Step 2: dmaState 2→0 (execute transfer)
     * Normally each step happens on successive CPU access cycles.
     * Since recompiled code bypasses the CPU, we call handleDma twice
     * to complete both steps immediately. */
    if (addr == 0x420B || addr == 0x420C) {
        dma_handleDma(snes->dma, 8);  /* state 1→2 (arm) */
        dma_handleDma(snes->dma, 8);  /* state 2→0 (execute) */
    }
}

uint16_t bus_read16(uint8_t bank, uint16_t addr) {
    uint8_t lo = bus_read8(bank, addr);
    uint16_t next_addr = addr + 1;
    uint8_t next_bank = bank;
    if (next_addr == 0x0000) {
        next_bank++;
    }
    uint8_t hi = bus_read8(next_bank, next_addr);
    return (uint16_t)(lo | (hi << 8));
}

void bus_write16(uint8_t bank, uint16_t addr, uint16_t val) {
    bus_write8(bank, addr, (uint8_t)(val & 0xFF));
    uint16_t next_addr = addr + 1;
    uint8_t next_bank = bank;
    if (next_addr == 0x0000) {
        next_bank++;
    }
    bus_write8(next_bank, next_addr, (uint8_t)(val >> 8));
}

/* --- Direct WRAM access (bypass bus for speed) --- */

uint8_t bus_wram_read8(uint32_t offset) {
    Snes *snes = snesrecomp_get_snes();
    if (!snes) return 0;
    return snes->ram[offset & 0x1FFFF];
}

void bus_wram_write8(uint32_t offset, uint8_t val) {
    Snes *snes = snesrecomp_get_snes();
    if (!snes) return;
    snes->ram[offset & 0x1FFFF] = val;
}

uint16_t bus_wram_read16(uint32_t offset) {
    uint8_t lo = bus_wram_read8(offset);
    uint8_t hi = bus_wram_read8(offset + 1);
    return (uint16_t)(lo | (hi << 8));
}

void bus_wram_write16(uint32_t offset, uint16_t val) {
    bus_wram_write8(offset, (uint8_t)(val & 0xFF));
    bus_wram_write8(offset + 1, (uint8_t)(val >> 8));
}

uint8_t *bus_get_wram(void) {
    Snes *snes = snesrecomp_get_snes();
    if (!snes) return NULL;
    return snes->ram;
}

const uint8_t *bus_get_rom(uint32_t *size_out) {
    Snes *snes = snesrecomp_get_snes();
    if (!snes || !snes->cart) return NULL;
    if (size_out) *size_out = snes->cart->romSize;
    return snes->cart->rom;
}

/* --- GSU / Super FX access --- */
/* Stub: upstream LakeSnes Cart doesn't have a gsu field.
 * These are no-ops for games that don't use Super FX. */

bool bus_has_gsu(void) { return false; }
uint8_t bus_gsu_read(uint16_t addr) { (void)addr; return 0; }
void bus_gsu_write(uint16_t addr, uint8_t val) { (void)addr; (void)val; }
void bus_gsu_run(void) { }
