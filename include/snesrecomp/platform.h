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

/*
 * Headless mode (call before platform_init / snesrecomp_init): no window,
 * renderer, audio device or frame pacing. Screenshots (SNESRECOMP_SHOT_FRAMES)
 * come from the PPU framebuffer; record_path, if non-NULL, pipes every frame
 * to ffmpeg (must be on PATH) as an H.264 mp4. snesrecomp_parse_args() wires
 * this to --headless / --record.
 */
void platform_set_headless(const char *record_path);
bool platform_is_headless(void);

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

/* Request a PNG screenshot of the next presented frame (same path as F12).
 * Written to screenshot_NNNN.png in the working directory. */
void platform_request_screenshot(void);

/*
 * Map the host cursor to SNES screen pixels (x: 0..255, y: 0..223), inverting
 * the same crop/scale/menu-bar offset platform_present_frame applies.
 *
 * Returns false when the pointer is outside the rendered game area (over the
 * menu bar, or off-window), in which case *x/*y are untouched.
 *
 * A SNES mouse is a relative device, so a port that just forwards deltas drifts
 * away from the host cursor and can never recover. With this, a game whose
 * cursor position is known can steer toward the host pointer each frame instead.
 */
bool platform_mouse_to_snes(int *x, int *y);

/* Clean shutdown */
void platform_shutdown(void);

#endif /* SNESRECOMP_PLATFORM_H */
