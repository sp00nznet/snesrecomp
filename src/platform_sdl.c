/*
 * SDL2 platform layer — window, renderer, audio output, frame timing.
 */

#include "snesrecomp/platform.h"
#include "snesrecomp/menu_overlay.h"
#include "snesrecomp/snesrecomp.h"
#include <SDL.h>
#include <stdio.h>

static SDL_Window   *s_window   = NULL;
static SDL_Renderer *s_renderer = NULL;
static SDL_Texture  *s_texture  = NULL;
static SDL_AudioDeviceID s_audio_dev = 0;
static uint64_t      s_frame_start = 0;
static int           s_cur_scale  = 0;   /* tracks live window-scale changes */
static int           s_cur_filter = -1;  /* tracks live texture-filter changes */

/* (Re)create the streaming game texture with the given filter (0=nearest,
 * 1=linear). Called at init and when the menu changes the filter. */
static void recreate_texture(int filter) {
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, filter ? "1" : "0");
    if (s_texture) SDL_DestroyTexture(s_texture);
    s_texture = SDL_CreateTexture(s_renderer,
        SDL_PIXELFORMAT_RGBX8888, SDL_TEXTUREACCESS_STREAMING,
        SNES_RENDER_WIDTH, SNES_RENDER_HEIGHT);
    s_cur_filter = filter;
}

/* NTSC frame time: ~16.6393 ms (60.098 Hz) */
static const double FRAME_TIME_MS = 1000.0 / 60.098;

bool platform_init(const char *window_title, int scale) {
    if (scale < 1) scale = 1;
    if (scale > 8) scale = 8;

    int win_w = SNES_RENDER_WIDTH * scale / 2;  /* 512 is 2x native, scale from 256 */
    int win_h = SNES_RENDER_HEIGHT * scale / 2;  /* 478 is ~2x native */

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
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

    s_cur_scale  = scale;
    s_cur_filter = 1;

    /* ImGui menu overlay (no-op in headless/scripted runs). Apply any persisted
     * scale/filter from its loaded config. */
    menu_overlay_init(s_window, s_renderer);
    if (menu_overlay_get_scale() != s_cur_scale) {
        s_cur_scale = menu_overlay_get_scale();
        SDL_SetWindowSize(s_window, SNES_RENDER_WIDTH * s_cur_scale / 2,
                                    SNES_RENDER_HEIGHT * s_cur_scale / 2);
    }
    if (menu_overlay_get_filter() != s_cur_filter) recreate_texture(menu_overlay_get_filter());

    s_frame_start = SDL_GetPerformanceCounter();
    return true;
}

void platform_present_frame(const uint8_t *framebuffer) {
    if (!s_texture || !framebuffer) return;

    /* Apply live graphics-setting changes from the menu. */
    int want_scale = menu_overlay_get_scale();
    if (want_scale != s_cur_scale) {
        s_cur_scale = want_scale;
        SDL_SetWindowSize(s_window, SNES_RENDER_WIDTH * s_cur_scale / 2,
                                    SNES_RENDER_HEIGHT * s_cur_scale / 2);
    }
    int want_filter = menu_overlay_get_filter();
    if (want_filter != s_cur_filter) recreate_texture(want_filter);

    SDL_UpdateTexture(s_texture, NULL, framebuffer,
                      SNES_RENDER_WIDTH * 4);

    /* Crop the PPU's black overscan bands (the active picture is centred in the
     * 478-tall buffer), and place the game below the menu bar so the menu never
     * covers the top-of-screen HUD (lap timer/position). */
    int rx, ry, rw, rh;
    snesrecomp_active_video_rect(&rx, &ry, &rw, &rh);
    SDL_Rect src = { rx, ry, rw, rh };
    int menu_h = menu_overlay_get_menubar_height();
    int out_w = 0, out_h = 0;
    SDL_GetRendererOutputSize(s_renderer, &out_w, &out_h);
    SDL_Rect dst = { 0, menu_h, out_w, out_h - menu_h };

    SDL_RenderClear(s_renderer);
    SDL_RenderCopy(s_renderer, s_texture, &src, &dst);
    menu_overlay_render(s_renderer);   /* ImGui menu on top (no-op when disabled) */
    SDL_RenderPresent(s_renderer);
}

void platform_queue_audio(const int16_t *samples, int sample_count) {
    if (s_audio_dev > 0 && samples && sample_count > 0) {
        /* Don't let audio buffer grow too large */
        uint32_t queued = SDL_GetQueuedAudioSize(s_audio_dev);
        if (queued < 32040 * 2 * 2 * 4) { /* ~4 frames max */
            float vol = menu_overlay_get_volume();   /* 1.0 when menu disabled */
            if (vol >= 0.999f) {
                SDL_QueueAudio(s_audio_dev, samples,
                               (uint32_t)(sample_count * 2 * sizeof(int16_t)));
            } else {
                /* Scale by master volume into a temp buffer. */
                static int16_t scaled[4096 * 2];
                int n = sample_count * 2;
                if (n > (int)(sizeof(scaled) / sizeof(scaled[0]))) n = sizeof(scaled) / sizeof(scaled[0]);
                for (int i = 0; i < n; i++) scaled[i] = (int16_t)((int)samples[i] * vol);
                SDL_QueueAudio(s_audio_dev, scaled, (uint32_t)(n * sizeof(int16_t)));
            }
        }
    }
}

/* Mouse motion accumulator (defined in input.c) */
extern void recomp_input_accumulate_mouse(int dx, int dy);

bool platform_poll_events(void) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        /* Let the ImGui menu see every event first; if it captured the event
         * (typing in a field, rebinding, clicking a menu) the game ignores it. */
        int consumed = menu_overlay_process_event(&ev);
        switch (ev.type) {
        case SDL_QUIT:
            return false;
        case SDL_KEYDOWN:
            /* Esc quits only when the menu isn't using it (e.g. rebind-cancel). */
            if (!consumed && ev.key.keysym.sym == SDLK_ESCAPE)
                return false;
            break;
        case SDL_MOUSEMOTION:
            if (!consumed)
                recomp_input_accumulate_mouse(ev.motion.xrel, ev.motion.yrel);
            break;
        default:
            break;
        }
    }
    if (menu_overlay_quit_requested()) return false;
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
    menu_overlay_shutdown();
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
