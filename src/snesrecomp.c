/*
 * snesrecomp core lifecycle — init, frame, shutdown.
 *
 * This is the glue that ties LakeSnes hardware, the recomp CPU state,
 * and the SDL2 platform layer together into one clean package.
 */

#include "snesrecomp/snesrecomp.h"
#include "snes.h"
#include "ppu.h"
#include "input.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The LakeSnes instance that provides all real SNES hardware */
static Snes *s_snes = NULL;

/* Pixel buffer for PPU output (512x478x4 = ~978 KB) */
static uint8_t s_pixel_buf[512 * 478 * 4];

/* Audio sample buffer (enough for one frame at 32040 Hz / 60 fps) */
#define SAMPLES_PER_FRAME 534
static int16_t s_audio_buf[SAMPLES_PER_FRAME * 2]; /* stereo */

Snes *snesrecomp_get_snes(void) {
    return s_snes;
}

uint16_t snesrecomp_read_vram(uint16_t word_addr) {
    if (!s_snes || !s_snes->ppu) return 0;
    return s_snes->ppu->vram[word_addr & 0x7FFF];
}

/*
 * Dump a compact binary snapshot of guest state (WRAM + VRAM) for lockstep
 * divergence analysis. Modeled on the snesrev zelda3 verification harness:
 * run the same ROM under two execution paths (pure interpreter as the
 * ground-truth reference vs. recompiled functions), snapshot every frame,
 * and diff to find the first WRAM/VRAM address that diverges — i.e. the
 * first incorrect recompiled function.
 *
 * Format (little-endian, matches the x86 host):
 *   char  magic[8]  = "SMKSNAP2"
 *   u32   wram_size = 0x20000
 *   u32   vram_size = 0x10000   (bytes; 0x8000 words)
 *   u32   cgram_size = 0x200    (bytes; 0x100 words)
 *   u8    wram[wram_size]
 *   u8    vram[vram_size]       (raw uint16 array, low byte first)
 *   u8    cgram[cgram_size]     (raw uint16 array, low byte first)
 *
 * CGRAM is included because Mode-7 BG1 garbling can be a palette fault as
 * easily as a char-data fault; comparing both against a Mesen2 reference dump
 * (tools/mesen/dump_snapshot.lua emits this exact format) isolates which.
 * The diff tool still reads the older "SMKSNAP1" (no cgram) for back-compat.
 */
void snesrecomp_dump_snapshot(const char *filepath) {
    if (!s_snes || !s_snes->ppu) return;
    FILE *f = fopen(filepath, "wb");
    if (!f) {
        fprintf(stderr, "snesrecomp: failed to open snapshot '%s'\n", filepath);
        return;
    }
    const uint32_t wram_size = 0x20000;
    const uint32_t vram_size = 0x10000;  /* 0x8000 words * 2 bytes */
    const uint32_t cgram_size = 0x200;   /* 0x100 words * 2 bytes */
    fwrite("SMKSNAP2", 1, 8, f);
    fwrite(&wram_size, sizeof(uint32_t), 1, f);
    fwrite(&vram_size, sizeof(uint32_t), 1, f);
    fwrite(&cgram_size, sizeof(uint32_t), 1, f);
    fwrite(s_snes->ram, 1, wram_size, f);
    fwrite(s_snes->ppu->vram, 1, vram_size, f);
    fwrite(s_snes->ppu->cgram, 1, cgram_size, f);
    fclose(f);
}

bool snesrecomp_init(const char *window_title, int scale) {
    /* Create LakeSnes instance (allocates CPU, PPU, APU, DMA, Cart) */
    s_snes = snes_init();
    if (!s_snes) {
        fprintf(stderr, "snesrecomp: failed to create SNES instance\n");
        return false;
    }

    /* Set pixel format to RGBX (matches SDL RGBX8888) */
    snes_setPixelFormat(s_snes, pixelFormatRGBX);

    /* Point PPU pixel output at our buffer */
    snes_setPixels(s_snes, s_pixel_buf);

    /* Initialize recomp CPU state */
    recomp_cpu_reset();

    /* Note: func_table_init() is intentionally NOT called here.
     * RECOMP_PATCH static constructors run before main() and populate the
     * dispatch table; init() would wipe them. The table is zero-initialized
     * by C static-storage semantics, so no explicit reset is needed.
     * Call func_table_init() yourself only if you need to clear the table. */

    /* Initialize SDL2 platform */
    if (!platform_init(window_title, scale)) {
        fprintf(stderr, "snesrecomp: failed to initialize platform\n");
        snes_free(s_snes);
        s_snes = NULL;
        return false;
    }

    printf("snesrecomp: initialized (LakeSnes backend)\n");
    return true;
}

bool snesrecomp_load_rom(const char *path) {
    if (!s_snes) return false;

    /* Read ROM file into memory */
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "snesrecomp: failed to open ROM: %s\n", path);
        return false;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t *data = malloc((size_t)size);
    if (!data) {
        fclose(f);
        return false;
    }

    size_t read = fread(data, 1, (size_t)size, f);
    fclose(f);

    if ((long)read != size) {
        fprintf(stderr, "snesrecomp: failed to read ROM (%zu of %ld bytes)\n", read, size);
        free(data);
        return false;
    }

    /* Let LakeSnes handle header detection, mapping type, region, etc. */
    bool ok = snes_loadRom(s_snes, data, (int)size);
    free(data);

    if (!ok) {
        fprintf(stderr, "snesrecomp: LakeSnes rejected the ROM\n");
        return false;
    }

    /* Re-point pixel output (reset clears it) */
    snes_setPixels(s_snes, s_pixel_buf);

    printf("snesrecomp: ROM loaded successfully\n");
    return true;
}

bool snesrecomp_begin_frame(void) {
    /* Poll SDL events */
    if (!platform_poll_events()) {
        return false;
    }

    /* Update input and feed into LakeSnes */
    recomp_input_update();

    /* Perform the auto-joypad read that real hardware does each vblank.
     * snesrecomp bypasses the LakeSnes per-cycle CPU loop (where this would
     * normally fire), so the auto-read registers $4218-$421F would otherwise
     * never be populated and recompiled code would see no input. */
    if (s_snes) snes_doAutoJoypad(s_snes);

    return true;
}

void snesrecomp_end_frame(void) {
    if (!s_snes) return;

    /* Start-of-frame PPU bookkeeping (toggles evenFrame, resets mosaic, etc.) */
    ppu_handleFrameStart(s_snes->ppu);
    s_snes->input2->scopeLatched = false; /* reset Super Scope latch for new frame */

    /* Initialize HDMA for this frame. snesrecomp renders scanlines manually and
     * never steps the per-cycle CPU loop where LakeSnes normally runs HDMA, so
     * we drive it here (untimed). Without this, per-scanline register effects
     * (Mode-7 BGMODE switching, matrix updates, window/color raster effects)
     * never apply — e.g. SMK's track plane never enters Mode 7. */
    if (s_snes->dma) dma_hdmaInitFrame(s_snes->dma);

    /* Diagnostic: when SMK_HDMA_DEBUG is set, on the first Mode-7 frame report
     * which registers the active HDMA channels target and CGRAM before/after
     * the scanline loop. Used to confirm whether per-scanline HDMA clobbers the
     * Mode-7 palette (CGRAM). One-shot. */
    static int s_hdma_dbg_done = 0;
    int hdma_dbg = getenv("SMK_HDMA_DEBUG") && !s_hdma_dbg_done &&
                   s_snes->ppu->mode == 7;
    if (hdma_dbg) {
        s_hdma_dbg_done = 1;
        fprintf(stderr, "[HDMA_DEBUG] mode=7 frame, active HDMA channels:\n");
        for (int c = 0; c < 8; c++) {
            if (s_snes->dma->channel[c].hdmaActive)
                fprintf(stderr, "  ch%d -> $21%02X (mode=%d aBank=%02X aAdr=%04X)\n",
                        c, s_snes->dma->channel[c].bAdr,
                        s_snes->dma->channel[c].mode,
                        s_snes->dma->channel[c].aBank,
                        s_snes->dma->channel[c].aAdr);
        }
        fprintf(stderr, "  CGRAM[0..7] BEFORE scanline HDMA: ");
        for (int i = 0; i < 8; i++) fprintf(stderr, "%04X ", s_snes->ppu->cgram[i]);
        fprintf(stderr, "\n");
    }

    /* Run all PPU scanlines for the frame */
    for (int line = 1; line <= 224; line++) {
        /* Apply this scanline's HDMA register writes before rendering it
         * (real hardware does HDMA during the preceding HBlank). */
        if (s_snes->dma) dma_hdmaRunLine(s_snes->dma);
        ppu_runLine(s_snes->ppu, line);

        /*
         * Super Scope beam detection: when the PPU renders the scanline
         * the scope is aimed at, trigger a counter latch. This simulates
         * the photodiode firing when the CRT beam passes the target.
         * snes_runCycle() isn't used in recomp, so we do it here.
         */
        if (s_snes->input2->type == inputDeviceSuperScope &&
            s_snes->ppuLatch && !s_snes->input2->scopeOffscreen &&
            !s_snes->input2->scopeLatched) {
            uint16_t targetV = s_snes->input2->scopeY + 1;
            if ((uint16_t)line == targetV) {
                s_snes->ppu->hCount = s_snes->input2->scopeX + 22;
                s_snes->ppu->vCount = targetV;
                s_snes->ppu->countersLatched = true;
                s_snes->input2->scopeLatched = true;
            }
        }
    }
    ppu_handleVblank(s_snes->ppu);

    if (hdma_dbg) {
        fprintf(stderr, "  CGRAM[0..7] AFTER  scanline HDMA: ");
        for (int i = 0; i < 8; i++) fprintf(stderr, "%04X ", s_snes->ppu->cgram[i]);
        fprintf(stderr, "\n");
    }

    /* Copy rendered pixels from PPU internal buffer to output buffer */
    snes_setPixels(s_snes, s_pixel_buf);

    /* Catch up APU audio and extract samples */
    int apu_cycles = (int)(s_snes->palTiming ? 534 : 534);
    apu_runCycles(s_snes->apu, apu_cycles * 32);
    snes_setSamples(s_snes, s_audio_buf, SAMPLES_PER_FRAME);

    /* Present video */
    platform_present_frame(s_pixel_buf);

    /* Queue audio */
    platform_queue_audio(s_audio_buf, SAMPLES_PER_FRAME);

    /* Frame sync */
    platform_frame_sync();
}

void snesrecomp_trigger_vblank(void) {
    if (!s_snes) return;
    s_snes->inVblank = true;
    s_snes->inNmi = true;
}

const uint8_t *snesrecomp_get_framebuffer(void) {
    return s_pixel_buf;
}

void snesrecomp_dump_ppu(const char *filepath) {
    FILE *dbg = fopen(filepath, "w");
    if (!dbg) return;
    fprintf(dbg, "s_snes=%p\n", (void*)s_snes);
    if (!s_snes) { fclose(dbg); return; }
    Ppu *ppu = s_snes->ppu;
    fprintf(dbg, "ppu=%p\n", (void*)ppu);
    if (!ppu) { fclose(dbg); return; }
    fprintf(dbg, "forcedBlank=%d brightness=%d mode=%d evenFrame=%d\n",
           ppu->forcedBlank, ppu->brightness, ppu->mode, ppu->evenFrame);
    fprintf(dbg, "M7 matrix a=%d b=%d c=%d d=%d x=%d y=%d h=%d v=%d  startX=%d startY=%d sel=%d extBg=%d\n",
           ppu->m7matrix[0], ppu->m7matrix[1], ppu->m7matrix[2], ppu->m7matrix[3],
           ppu->m7matrix[4], ppu->m7matrix[5], ppu->m7matrix[6], ppu->m7matrix[7],
           ppu->m7startX, ppu->m7startY, ppu->m7largeField, ppu->m7extBg);
    fprintf(dbg, "layer mainScreenEnabled: %d %d %d %d %d\n",
           ppu->layer[0].mainScreenEnabled, ppu->layer[1].mainScreenEnabled,
           ppu->layer[2].mainScreenEnabled, ppu->layer[3].mainScreenEnabled,
           ppu->layer[4].mainScreenEnabled);
    fprintf(dbg, "BG tilemapAdr: %04X %04X %04X %04X\n",
           ppu->bgLayer[0].tilemapAdr, ppu->bgLayer[1].tilemapAdr,
           ppu->bgLayer[2].tilemapAdr, ppu->bgLayer[3].tilemapAdr);
    fprintf(dbg, "BG tileAdr: %04X %04X %04X %04X\n",
           ppu->bgLayer[0].tileAdr, ppu->bgLayer[1].tileAdr,
           ppu->bgLayer[2].tileAdr, ppu->bgLayer[3].tileAdr);
    fprintf(dbg, "CGRAM[0-7]: %04X %04X %04X %04X %04X %04X %04X %04X\n",
           ppu->cgram[0], ppu->cgram[1], ppu->cgram[2], ppu->cgram[3],
           ppu->cgram[4], ppu->cgram[5], ppu->cgram[6], ppu->cgram[7]);
    fprintf(dbg, "VRAM[0-7]: %04X %04X %04X %04X %04X %04X %04X %04X\n",
           ppu->vram[0], ppu->vram[1], ppu->vram[2], ppu->vram[3],
           ppu->vram[4], ppu->vram[5], ppu->vram[6], ppu->vram[7]);
    /* Dump VRAM at key tilemap addresses */
    fprintf(dbg, "VRAM[$1000-$1007]: %04X %04X %04X %04X %04X %04X %04X %04X\n",
           ppu->vram[0x1000], ppu->vram[0x1001], ppu->vram[0x1002], ppu->vram[0x1003],
           ppu->vram[0x1004], ppu->vram[0x1005], ppu->vram[0x1006], ppu->vram[0x1007]);
    fprintf(dbg, "VRAM[$1400-$1407]: %04X %04X %04X %04X %04X %04X %04X %04X\n",
           ppu->vram[0x1400], ppu->vram[0x1401], ppu->vram[0x1402], ppu->vram[0x1403],
           ppu->vram[0x1400+4], ppu->vram[0x1400+5], ppu->vram[0x1400+6], ppu->vram[0x1400+7]);
    fprintf(dbg, "VRAM[$C000-$C007]: %04X %04X %04X %04X %04X %04X %04X %04X\n",
           ppu->vram[0xC000&0x7FFF], ppu->vram[(0xC001)&0x7FFF], ppu->vram[(0xC002)&0x7FFF], ppu->vram[(0xC003)&0x7FFF],
           ppu->vram[(0xC004)&0x7FFF], ppu->vram[(0xC005)&0x7FFF], ppu->vram[(0xC006)&0x7FFF], ppu->vram[(0xC007)&0x7FFF]);
    /* VRAM size check */
    int vram_nonzero = 0;
    for (int i = 0; i < 0x8000; i++) {
        if (ppu->vram[i] != 0) vram_nonzero++;
    }
    fprintf(dbg, "VRAM total nonzero words: %d / 32768\n", vram_nonzero);
    /* VRAM heatmap: count nonzero words in each 1K-word block */
    fprintf(dbg, "VRAM heatmap (1K-word blocks, word addr):\n");
    for (int blk = 0; blk < 32; blk++) {
        int cnt = 0;
        for (int j = 0; j < 0x400; j++) {
            if (ppu->vram[blk * 0x400 + j] != 0) cnt++;
        }
        if (cnt > 0) {
            fprintf(dbg, "  $%04X-$%04X: %4d / 1024 nonzero\n",
                   blk * 0x400, blk * 0x400 + 0x3FF, cnt);
        }
    }
    /* OBJ settings */
    fprintf(dbg, "objSize=%d objTileAdr1=$%04X objTileAdr2=$%04X\n",
           ppu->objSize, ppu->objTileAdr1, ppu->objTileAdr2);
    /* OAM entries (first 16 sprites) */
    fprintf(dbg, "OAM (first 16 sprites):\n");
    for (int i = 0; i < 16; i++) {
        int idx = i * 2;
        uint16_t w0 = ppu->oam[idx];
        uint16_t w1 = ppu->oam[idx + 1];
        uint8_t x = w0 & 0xFF;
        uint8_t y = w0 >> 8;
        uint8_t tile = w1 & 0xFF;
        int nt = (w1 >> 8) & 1;
        int pal = (w1 >> 9) & 7;
        int pri = (w1 >> 12) & 3;
        int hf = (w1 >> 14) & 1;
        int vf = (w1 >> 15) & 1;
        /* High OAM: size + X bit 9 */
        int hi_byte = ppu->highOam[idx >> 3];
        int x9 = (hi_byte >> (idx & 7)) & 1;
        int sz = (hi_byte >> ((idx & 7) + 1)) & 1;
        fprintf(dbg, "  [%2d] x=%3d%s y=%3d tile=$%02X nt=%d pal=%d pri=%d hf=%d vf=%d sz=%d\n",
               i, x | (x9 << 8), x9 ? "+" : " ", y, tile, nt, pal, pri, hf, vf, sz);
    }
    /* CGRAM OBJ palette (entries 128-143 = OBJ palette 0) */
    for (int p = 0; p < 8; p++) {
        fprintf(dbg, "CGRAM OBJ pal %d (%d-%d): ", p, 128+p*16, 128+p*16+15);
        for (int i = 128+p*16; i < 128+p*16+16; i++) {
            fprintf(dbg, "%04X ", ppu->cgram[i]);
        }
        fprintf(dbg, "\n");
    }
    /* VRAM at sprite tile base ($4000) - first 16 words */
    fprintf(dbg, "VRAM[$4000-$400F]: ");
    for (int i = 0; i < 16; i++) {
        fprintf(dbg, "%04X ", ppu->vram[0x4000 + i]);
    }
    fprintf(dbg, "\n");
    /* VRAM at DMA'd sprite tile locations (per slot) */
    {
        static const uint16_t slot_vram[] = {0x5800,0x5840,0x5880,0x58C0,0x5C00,0x5C40,0x5C80,0x5CC0};
        for (int s = 0; s < 8; s++) {
            uint16_t base = slot_vram[s];
            int nz = 0;
            for (int w = 0; w < 16; w++) {
                if (ppu->vram[base + w] != 0) nz++;
            }
            fprintf(dbg, "VRAM[$%04X] slot%d: %d/16 nonzero", base, s, nz);
            if (nz > 0) {
                fprintf(dbg, " first4: %04X %04X %04X %04X",
                       ppu->vram[base], ppu->vram[base+1], ppu->vram[base+2], ppu->vram[base+3]);
            }
            fprintf(dbg, "\n");
        }
    }
    /* WRAM OAM staging buffer at $0200 (first 16 bytes) */
    fprintf(dbg, "WRAM[$0200-$020F]: ");
    for (int i = 0; i < 16; i++) {
        fprintf(dbg, "%02X ", s_snes->ram[0x0200 + i]);
    }
    fprintf(dbg, "\n");
    /* WRAM OAM "PUSH START" region at $03D0 (48 bytes = 12 sprites) */
    fprintf(dbg, "WRAM[$03D0-$03FF]: ");
    for (int i = 0; i < 48; i++) {
        fprintf(dbg, "%02X ", s_snes->ram[0x03D0 + i]);
    }
    fprintf(dbg, "\n");
    /* WRAM OAM high table at $0400 (32 bytes) */
    fprintf(dbg, "WRAM[$0400-$041F]: ");
    for (int i = 0; i < 32; i++) {
        fprintf(dbg, "%02X ", s_snes->ram[0x0400 + i]);
    }
    fprintf(dbg, "\n");
    /* PPU OAM entries 116-127 */
    fprintf(dbg, "OAM (sprites 116-127):\n");
    for (int i = 116; i < 128; i++) {
        int idx = i * 2;
        uint16_t w0 = ppu->oam[idx];
        uint16_t w1 = ppu->oam[idx + 1];
        uint8_t x = w0 & 0xFF;
        uint8_t y = w0 >> 8;
        uint8_t tile = w1 & 0xFF;
        int nt = (w1 >> 8) & 1;
        int pal = (w1 >> 9) & 7;
        int pri = (w1 >> 12) & 3;
        int hf = (w1 >> 14) & 1;
        int vf = (w1 >> 15) & 1;
        int hi_byte = ppu->highOam[idx >> 3];
        int x9 = (hi_byte >> (idx & 7)) & 1;
        int sz = (hi_byte >> ((idx & 7) + 1)) & 1;
        fprintf(dbg, "  [%3d] x=%3d%s y=%3d tile=$%02X nt=%d pal=%d pri=%d hf=%d vf=%d sz=%d\n",
               i, x | (x9 << 8), x9 ? "+" : " ", y, tile, nt, pal, pri, hf, vf, sz);
    }
    /* pixelBuffer check */
    int pb_nonzero = 0;
    for (int i = 0; i < 512 * 2 * 239 * 4; i += 101) {
        if (ppu->pixelBuffer[i] != 0) pb_nonzero++;
    }
    fprintf(dbg, "pixelBuffer nonzero: %d\n", pb_nonzero);
    int sb_nonzero = 0;
    for (int i = 0; i < 512 * 478 * 4; i += 101) {
        if (s_pixel_buf[i] != 0) sb_nonzero++;
    }
    fprintf(dbg, "s_pixel_buf nonzero: %d\n", sb_nonzero);
    fclose(dbg);

    /* Save BMP screenshot alongside the PPU dump */
    {
        /* Build BMP path from filepath: replace extension with .bmp */
        char bmp_path[512];
        strncpy(bmp_path, filepath, sizeof(bmp_path) - 1);
        bmp_path[sizeof(bmp_path) - 1] = '\0';
        char *dot = strrchr(bmp_path, '.');
        if (dot) strcpy(dot, ".bmp");
        else strcat(bmp_path, ".bmp");

        /* SNES renders 256x224 (or 512x224 in hi-res, but PPU buffer is 512 wide).
         * s_pixel_buf is 512x478x4 (RGBX). We save the left 256 pixels of 224 lines. */
        int w = 256, h = 224;
        FILE *bmp = fopen(bmp_path, "wb");
        if (bmp) {
            /* BMP header (54 bytes) */
            int row_stride = w * 3;
            int pad = (4 - (row_stride % 4)) % 4;
            int data_size = (row_stride + pad) * h;
            int file_size = 54 + data_size;
            uint8_t hdr[54];
            memset(hdr, 0, 54);
            hdr[0] = 'B'; hdr[1] = 'M';
            hdr[2] = file_size & 0xFF; hdr[3] = (file_size >> 8) & 0xFF;
            hdr[4] = (file_size >> 16) & 0xFF; hdr[5] = (file_size >> 24) & 0xFF;
            hdr[10] = 54; /* pixel data offset */
            hdr[14] = 40; /* DIB header size */
            hdr[18] = w & 0xFF; hdr[19] = (w >> 8) & 0xFF;
            hdr[22] = h & 0xFF; hdr[23] = (h >> 8) & 0xFF;
            hdr[26] = 1; /* planes */
            hdr[28] = 24; /* bits per pixel */
            hdr[34] = data_size & 0xFF; hdr[35] = (data_size >> 8) & 0xFF;
            hdr[36] = (data_size >> 16) & 0xFF; hdr[37] = (data_size >> 24) & 0xFF;
            fwrite(hdr, 1, 54, bmp);

            /* BMP stores bottom-up, BGR. */
            uint8_t pad_bytes[3] = {0, 0, 0};
            for (int y = h - 1; y >= 0; y--) {
                for (int x = 0; x < w; x++) {
                    int idx = y * 2048 + x * 8;
                    uint8_t bgr[3];
                    bgr[0] = s_pixel_buf[idx + 1]; /* B */
                    bgr[1] = s_pixel_buf[idx + 2]; /* G */
                    bgr[2] = s_pixel_buf[idx + 3]; /* R */
                    fwrite(bgr, 1, 3, bmp);
                }
                if (pad > 0) fwrite(pad_bytes, 1, pad, bmp);
            }
            fclose(bmp);
        }
    }
}

void snesrecomp_shutdown(void) {
    platform_shutdown();

    if (s_snes) {
        snes_free(s_snes);
        s_snes = NULL;
    }

    printf("snesrecomp: shutdown complete\n");
}
