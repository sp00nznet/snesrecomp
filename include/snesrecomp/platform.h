#ifndef SNESRECOMP_PLATFORM_H
#define SNESRECOMP_PLATFORM_H

/*
 * SDL2 platform layer — windowing, rendering, audio output, frame timing.
 *
 * You usually don't call these directly — snesrecomp_init/end_frame
 * handle it for you. But they're here if you need finer control.
 */

#include <stdbool.h>
#include <stdint.h>

/* SNES output resolution */
#define SNES_RENDER_WIDTH   512
#define SNES_RENDER_HEIGHT  478

/* Initialize SDL2 window + renderer + audio */
bool platform_init(const char *window_title, int scale);

/* Present a framebuffer (512x478 RGBX8888) */
void platform_present_frame(const uint8_t *framebuffer);

/* Queue audio samples (interleaved stereo int16) */
void platform_queue_audio(const int16_t *samples, int sample_count);

/* Poll SDL events. Returns false if quit was requested. */
bool platform_poll_events(void);

/* Wait to maintain ~60 Hz frame timing */
void platform_frame_sync(void);

/* Clean shutdown */
void platform_shutdown(void);

#endif /* SNESRECOMP_PLATFORM_H */
