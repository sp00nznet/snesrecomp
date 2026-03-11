/*
 * snesrecomp core lifecycle — init, frame, shutdown.
 *
 * This is the glue that ties LakeSnes hardware, the recomp CPU state,
 * and the SDL2 platform layer together into one clean package.
 */

#include "snesrecomp/snesrecomp.h"
#include "snes.h"

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

    /* Initialize function dispatch table */
    func_table_init();

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

    return true;
}

void snesrecomp_end_frame(void) {
    if (!s_snes) return;

    /* Run all PPU scanlines for the frame */
    for (int line = 1; line <= 224; line++) {
        ppu_runLine(s_snes->ppu, line);
    }
    ppu_handleVblank(s_snes->ppu);

    /* Catch up APU audio and extract samples */
    /* Run enough APU cycles for one frame */
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

void snesrecomp_shutdown(void) {
    platform_shutdown();

    if (s_snes) {
        snes_free(s_snes);
        s_snes = NULL;
    }

    printf("snesrecomp: shutdown complete\n");
}
