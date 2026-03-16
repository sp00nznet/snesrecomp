/*
 * SDL2 platform layer — window, renderer, audio output, frame timing.
 */

#include "snesrecomp/platform.h"
#include <SDL.h>
#include <stdio.h>

static SDL_Window   *s_window   = NULL;
static SDL_Renderer *s_renderer = NULL;
static SDL_Texture  *s_texture  = NULL;
static SDL_AudioDeviceID s_audio_dev = 0;
static uint64_t      s_frame_start = 0;

/* NTSC frame time: ~16.6393 ms (60.098 Hz) */
static const double FRAME_TIME_MS = 1000.0 / 60.098;

bool platform_init(const char *window_title, int scale) {
    if (scale < 1) scale = 1;
    if (scale > 8) scale = 8;

    int win_w = SNES_RENDER_WIDTH * scale / 2;  /* 512 is 2x native, scale from 256 */
    int win_h = SNES_RENDER_HEIGHT * scale / 2;  /* 478 is ~2x native */

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }

    s_window = SDL_CreateWindow(
        window_title ? window_title : "snesrecomp",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        win_w, win_h,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE
    );
    if (!s_window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return false;
    }

    s_renderer = SDL_CreateRenderer(s_window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!s_renderer) {
        fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(s_window);
        SDL_Quit();
        return false;
    }

    /* Use linear filtering for scaling */
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");

    s_texture = SDL_CreateTexture(s_renderer,
        SDL_PIXELFORMAT_RGBX8888,
        SDL_TEXTUREACCESS_STREAMING,
        SNES_RENDER_WIDTH, SNES_RENDER_HEIGHT);
    if (!s_texture) {
        fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
        SDL_DestroyRenderer(s_renderer);
        SDL_DestroyWindow(s_window);
        SDL_Quit();
        return false;
    }

    /* Initialize audio */
    SDL_AudioSpec want, have;
    SDL_memset(&want, 0, sizeof(want));
    want.freq = 32040;       /* SNES native sample rate */
    want.format = AUDIO_S16;
    want.channels = 2;
    want.samples = 1024;
    want.callback = NULL;    /* Push mode */

    s_audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (s_audio_dev > 0) {
        SDL_PauseAudioDevice(s_audio_dev, 0);  /* Start playback */
    }

    SDL_SetRenderDrawColor(s_renderer, 0, 0, 0, 255);
    SDL_RenderClear(s_renderer);
    SDL_RenderPresent(s_renderer);

    s_frame_start = SDL_GetPerformanceCounter();
    return true;
}

void platform_present_frame(const uint8_t *framebuffer) {
    if (!s_texture || !framebuffer) return;

    SDL_UpdateTexture(s_texture, NULL, framebuffer,
                      SNES_RENDER_WIDTH * 4);
    SDL_RenderClear(s_renderer);
    SDL_RenderCopy(s_renderer, s_texture, NULL, NULL);
    SDL_RenderPresent(s_renderer);
}

void platform_queue_audio(const int16_t *samples, int sample_count) {
    if (s_audio_dev > 0 && samples && sample_count > 0) {
        /* Don't let audio buffer grow too large */
        uint32_t queued = SDL_GetQueuedAudioSize(s_audio_dev);
        if (queued < 32040 * 2 * 2 * 4) { /* ~4 frames max */
            SDL_QueueAudio(s_audio_dev, samples,
                           (uint32_t)(sample_count * 2 * sizeof(int16_t)));
        }
    }
}

/* Mouse motion accumulator (defined in input.c) */
extern void recomp_input_accumulate_mouse(int dx, int dy);

bool platform_poll_events(void) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
        case SDL_QUIT:
            return false;
        case SDL_KEYDOWN:
            if (ev.key.keysym.sym == SDLK_ESCAPE)
                return false;
            break;
        case SDL_MOUSEMOTION:
            recomp_input_accumulate_mouse(ev.motion.xrel, ev.motion.yrel);
            break;
        default:
            break;
        }
    }
    return true;
}

void platform_frame_sync(void) {
    uint64_t now = SDL_GetPerformanceCounter();
    double elapsed_ms = (double)(now - s_frame_start) * 1000.0
                        / (double)SDL_GetPerformanceFrequency();
    double remaining = FRAME_TIME_MS - elapsed_ms;
    if (remaining > 1.0) {
        SDL_Delay((uint32_t)(remaining - 0.5));
    }
    s_frame_start = SDL_GetPerformanceCounter();
}

void platform_shutdown(void) {
    if (s_audio_dev > 0) {
        SDL_CloseAudioDevice(s_audio_dev);
        s_audio_dev = 0;
    }
    if (s_texture)  SDL_DestroyTexture(s_texture);
    if (s_renderer) SDL_DestroyRenderer(s_renderer);
    if (s_window)   SDL_DestroyWindow(s_window);
    SDL_Quit();
    s_texture  = NULL;
    s_renderer = NULL;
    s_window   = NULL;
}
