/*
 * snesrecomp — SNES Hardware Backend Library
 * https://github.com/sp00nznet/snesrecomp
 *
 * Drop-in SNES hardware for your static recompilation project.
 * Powered by LakeSnes (MIT) — real PPU rendering, SPC700 audio,
 * DMA, Mode 7, the whole deal. You bring the recompiled game code,
 * we bring the hardware.
 *
 * Usage:
 *   #include <snesrecomp/snesrecomp.h>
 *
 *   snesrecomp_init("My SNES Recomp", 3);
 *   snesrecomp_load_rom("game.sfc");
 *   while (running) {
 *       // your recompiled game frame logic here
 *       bus_write8(0x00, 0x2100, value);  // writes hit real PPU
 *       snesrecomp_begin_frame();
 *       // ... run recompiled functions ...
 *       snesrecomp_end_frame();           // renders + presents
 *   }
 *   snesrecomp_shutdown();
 */

#ifndef SNESRECOMP_H
#define SNESRECOMP_H

#include "cpu.h"
#include "cpu_ops.h"
#include "bus.h"
#include "func_table.h"
#include "recomp_patch.h"
#include "platform.h"
#include "input.h"

#include <stdbool.h>

/*
 * Initialize all SNES hardware subsystems and the SDL2 platform layer.
 * Call this once at startup before anything else.
 *
 * window_title: displayed in the SDL2 window title bar
 * scale:        integer scale factor (e.g. 3 = 768x672 window)
 */
bool snesrecomp_init(const char *window_title, int scale);

/*
 * Load a ROM file (.sfc / .smc). Handles copier headers, LoROM/HiROM/ExHiROM
 * auto-detection, and PAL/NTSC region detection automatically.
 * Returns true on success.
 */
bool snesrecomp_load_rom(const char *path);

/*
 * Call at the start of each frame before running recompiled game code.
 * Polls input, updates joypad auto-read, and prepares the frame.
 * Returns false if the user requested quit (window close / Escape).
 */
bool snesrecomp_begin_frame(void);

/*
 * Call at the end of each frame after running recompiled game code.
 * Renders all PPU scanlines, extracts audio samples, presents the
 * framebuffer via SDL2, and syncs to ~60 Hz NTSC timing.
 */
void snesrecomp_end_frame(void);

/*
 * Trigger VBlank processing — NMI flag, PPU vblank handling.
 * Call this when your recompiled code reaches the NMI/VBlank point.
 */
void snesrecomp_trigger_vblank(void);

/*
 * Get a pointer to the rendered framebuffer (512x478 RGBX8888).
 * Valid after snesrecomp_end_frame().
 */
const uint8_t *snesrecomp_get_framebuffer(void);

/*
 * Read a VRAM word at the given word address (0-32767).
 */
uint16_t snesrecomp_read_vram(uint16_t word_addr);

/*
 * Dump PPU diagnostic info to a file. For debugging rendering issues.
 */
void snesrecomp_dump_ppu(const char *filepath);

/*
 * Dump a compact binary snapshot of guest WRAM + VRAM for lockstep
 * divergence analysis (see tools/diff_snapshots.py). Used to compare a
 * pure-interpreter reference run against a recompiled run frame-by-frame.
 */
void snesrecomp_dump_snapshot(const char *filepath);

/*
 * Clean shutdown — frees all SNES hardware and closes SDL2.
 */
void snesrecomp_shutdown(void);

#endif /* SNESRECOMP_H */
