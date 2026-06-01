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
#include "apu.h"

#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

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

    /* Diagnostic: count/trace ALL CGRAM-data writes (CPU interp + recompiled C
     * both pass through here), to find non-interpreter CGRAM writers. */
    if (addr == 0x2122 && getenv("SMK_CG_DEBUG")) {
        static int n = 0;
        fprintf(stderr, "[CG] #%d $2122 <- %02X (bank=%02X)\n", n++, val, bank);
    }

    /* Diagnostic: trace the DMA channel-0 SETUP registers + trigger with the
     * CPU PC, to see whether the game performs per-DMA setup ($4302 srcL,
     * $4303 srcH, $4304 srcBank, $4305/6 size, $2121 CGADD) before each $420B,
     * or fires $420B with stale channel regs (race-start palette bug). */
    if (getenv("SMK_DMASETUP_DEBUG")) {
        uint16_t pc = (snes->cpu ? snes->cpu->pc : 0);
        uint8_t  k  = (snes->cpu ? snes->cpu->k : 0);
        if (addr == 0x2121 || addr == 0x4300 || addr == 0x4301 ||
            addr == 0x4302 || addr == 0x4303 ||
            addr == 0x4304 || addr == 0x4305 || addr == 0x4306 ||
            addr == 0x420B)
            fprintf(stderr, "[SET] $%04X <- %02X  pc=%02X:%04X\n",
                    addr, val, k, pc);
    }

    /* DMA fix: LakeSnes uses a 2-step state machine for DMA:
     *   Step 1: dmaState 1→2 (arm)
     *   Step 2: dmaState 2→0 (execute transfer)
     * Normally each step happens on successive CPU access cycles.
     * Since recompiled code bypasses the CPU, we call handleDma twice
     * to complete both steps immediately. */
    if (addr == 0x420B || addr == 0x420C) {
        /* Diagnostic: log GPDMA channel params (esp. CGRAM-targeting) so the
         * race-start palette-corrupting transfer can be identified. */
        if (addr == 0x420B && getenv("SMK_DMA_DEBUG")) {
            for (int c = 0; c < 8; c++) {
                if (!(val & (1 << c))) continue;
                fprintf(stderr, "[DMA] ch%d $420B=%02X -> $21%02X  src=%02X:%04X size=%04X mode=%d\n",
                        c, val, snes->dma->channel[c].bAdr,
                        snes->dma->channel[c].aBank, snes->dma->channel[c].aAdr,
                        snes->dma->channel[c].size, snes->dma->channel[c].mode);
            }
        }
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

void bus_apu_write_ram(uint16_t spc_addr, uint8_t val) {
    Snes *snes = snesrecomp_get_snes();
    if (!snes || !snes->apu) return;
    snes->apu->ram[spc_addr] = val;
}

uint8_t bus_apu_read_ram(uint16_t spc_addr) {
    Snes *snes = snesrecomp_get_snes();
    if (!snes || !snes->apu) return 0;
    return snes->apu->ram[spc_addr];
}

void bus_run_cycles(int master_cycles) {
    Snes *snes = snesrecomp_get_snes();
    if (!snes || !snes->apu) return;
    /* Convert master cycles to APU cycles.
     * NTSC: APU runs at ~1.024 MHz, master clock ~21.477 MHz.
     * Ratio is roughly 1 APU cycle per 21 master cycles,
     * but LakeSnes uses fractional accumulation internally.
     * We directly run the APU for an equivalent number of cycles. */
    int apu_cycles = master_cycles;  /* 1:1 works — apu_runCycles counts SPC clocks */
    apu_runCycles(snes->apu, apu_cycles);
}

