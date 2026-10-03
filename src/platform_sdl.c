/*
 * SDL2 platform layer — window, renderer, audio output, frame timing,
 * F12 screenshot with toast notification.
 */

#include "snesrecomp/platform.h"
#include "snesrecomp/menu_overlay.h"
#include "snesrecomp/input.h"
#include "snesrecomp/snesrecomp.h"
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stb_image_write.h"

#ifdef SNESRECOMP_HAS_TTF
#include <SDL_ttf.h>
#endif

static SDL_Window   *s_window   = NULL;
static SDL_Renderer *s_renderer = NULL;
static SDL_Texture  *s_texture  = NULL;
static SDL_AudioDeviceID s_audio_dev = 0;
static uint64_t      s_frame_start = 0;
static int           s_cur_scale  = 0;   /* tracks live window-scale changes */
static int           s_cur_filter = -1;  /* tracks live texture-filter changes */

/* Headless mode: no window, renderer or audio device, no 60 Hz pacing. Frames
 * still run; screenshots come straight from the PPU framebuffer and --record
 * pipes the same pixels to ffmpeg. Works over RDP without taking over a screen. */
static bool          s_headless    = false;
static const char   *s_record_path = NULL;
static FILE         *s_record_pipe = NULL;
static int           s_rec_w = 0, s_rec_h = 0;

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

/* Screenshot state */
static int  s_screenshot_counter = 0;
static bool s_screenshot_requested = false;

/* Toast notification state */
static char         s_toast_msg[256] = {0};
static Uint32       s_toast_start = 0;
static SDL_Texture *s_toast_tex = NULL;
static int          s_toast_w = 0;
static int          s_toast_h = 0;
#define TOAST_DURATION_MS  2500
#define TOAST_FADE_MS       500
#define TOAST_PADDING       8

#ifdef SNESRECOMP_HAS_TTF
static TTF_Font *s_font = NULL;
#endif

/* NTSC frame time: ~16.6393 ms (60.098 Hz) */
static const double FRAME_TIME_MS = 1000.0 / 60.098;

static void show_toast(const char *msg) {
    snprintf(s_toast_msg, sizeof(s_toast_msg), "%s", msg);
    s_toast_start = SDL_GetTicks();

    /* Destroy previous toast texture */
    if (s_toast_tex) {
        SDL_DestroyTexture(s_toast_tex);
        s_toast_tex = NULL;
    }

#ifdef SNESRECOMP_HAS_TTF
    if (s_font && s_renderer) {
        SDL_Color white = {255, 255, 255, 255};
        SDL_Surface *surf = TTF_RenderText_Blended(s_font, msg, white);
        if (surf) {
            s_toast_tex = SDL_CreateTextureFromSurface(s_renderer, surf);
            s_toast_w = surf->w;
            s_toast_h = surf->h;
            SDL_FreeSurface(surf);
            SDL_SetTextureBlendMode(s_toast_tex, SDL_BLENDMODE_BLEND);
        }
    }
#endif
}

static void take_screenshot(void) {
    int w, h;
    SDL_GetRendererOutputSize(s_renderer, &w, &h);

    uint8_t *pixels = (uint8_t *)malloc(w * h * 4);
    if (!pixels) return;

    if (SDL_RenderReadPixels(s_renderer, NULL, SDL_PIXELFORMAT_RGBA32,
                             pixels, w * 4) == 0) {
        char filename[512];
        snprintf(filename, sizeof(filename), "screenshot_%04d.png",
                 s_screenshot_counter++);
        stbi_write_png(filename, w, h, 4, pixels, w * 4);
        printf("Screenshot saved: %s\n", filename);
        fflush(stdout);
        show_toast(filename);
    }
    free(pixels);
}

static void draw_toast(void) {
    if (s_toast_msg[0] == '\0') return;

    Uint32 elapsed = SDL_GetTicks() - s_toast_start;
    if (elapsed >= TOAST_DURATION_MS) {
        s_toast_msg[0] = '\0';
        if (s_toast_tex) {
            SDL_DestroyTexture(s_toast_tex);
            s_toast_tex = NULL;
        }
        return;
    }

    /* Compute alpha (fade out in last TOAST_FADE_MS) */
    float alpha = 1.0f;
    if (elapsed > TOAST_DURATION_MS - TOAST_FADE_MS) {
        alpha = (float)(TOAST_DURATION_MS - elapsed) / (float)TOAST_FADE_MS;
    }

    int win_w, win_h;
    SDL_GetRendererOutputSize(s_renderer, &win_w, &win_h);

    SDL_SetRenderDrawBlendMode(s_renderer, SDL_BLENDMODE_BLEND);

    if (s_toast_tex) {
        /* Draw background box */
        int box_w = s_toast_w + TOAST_PADDING * 2;
        int box_h = s_toast_h + TOAST_PADDING * 2;
        int box_x = win_w - box_w - TOAST_PADDING;
        int box_y = win_h - box_h - TOAST_PADDING;

        SDL_Rect bg = { box_x, box_y, box_w, box_h };
        SDL_SetRenderDrawColor(s_renderer, 0, 0, 0, (Uint8)(180 * alpha));
        SDL_RenderFillRect(s_renderer, &bg);

        /* Draw text */
        SDL_Rect dst = { box_x + TOAST_PADDING, box_y + TOAST_PADDING,
                         s_toast_w, s_toast_h };
        SDL_SetTextureAlphaMod(s_toast_tex, (Uint8)(255 * alpha));
        SDL_RenderCopy(s_renderer, s_toast_tex, NULL, &dst);
    } else {
        /* Fallback: no TTF — draw a simple bar */
        int bar_h = 20;
        SDL_Rect bar = { 0, win_h - bar_h, win_w, bar_h };
        SDL_SetRenderDrawColor(s_renderer, 40, 40, 40, (Uint8)(200 * alpha));
        SDL_RenderFillRect(s_renderer, &bar);
    }
}

void platform_set_headless(const char *record_path) {
    s_headless = true;
    s_record_path = record_path;
}

bool platform_is_headless(void) { return s_headless; }

/* Active picture (overscan cropped) from the PPU buffer: bytes are X,B,G,R. */
static void headless_capture(const uint8_t *fb) {
    int rx, ry, rw, rh;
    snesrecomp_active_video_rect(&rx, &ry, &rw, &rh);
    const uint8_t *src = fb + (size_t)ry * SNES_RENDER_WIDTH * 4 + rx * 4;

    if (s_screenshot_requested) {
        s_screenshot_requested = false;
        uint8_t *rgba = (uint8_t *)malloc((size_t)rw * rh * 4);
        if (rgba) {
            for (int y = 0; y < rh; y++)
                for (int x = 0; x < rw; x++) {
                    const uint8_t *p = src + ((size_t)y * SNES_RENDER_WIDTH + x) * 4;
                    uint8_t *q = rgba + ((size_t)y * rw + x) * 4;
                    q[0] = p[3]; q[1] = p[2]; q[2] = p[1]; q[3] = 255;
                }
            char filename[64];
            snprintf(filename, sizeof(filename), "screenshot_%04d.png", s_screenshot_counter++);
            stbi_write_png(filename, rw, rh, 4, rgba, rw * 4);
            printf("Screenshot saved: %s\n", filename);
            fflush(stdout);
            free(rgba);
        }
    }

    if (!s_record_path) return;
    if (!s_record_pipe) {
        /* ponytail: frame size fixed by the first frame; a later overscan switch
         * is cropped/padded to it rather than restarting the encoder. */
        s_rec_w = rw; s_rec_h = rh;
        char cmd[1024];
        snprintf(cmd, sizeof(cmd),
            "ffmpeg -y -loglevel error -f rawvideo -pix_fmt abgr -s %dx%d -r 60.098 -i - "
            "-c:v libx264 -pix_fmt yuv420p \"%s\"", s_rec_w, s_rec_h, s_record_path);
#ifdef _WIN32
        s_record_pipe = _popen(cmd, "wb");
#else
        s_record_pipe = popen(cmd, "w");
#endif
        if (!s_record_pipe) {
            fprintf(stderr, "platform: could not start ffmpeg for --record (is it on PATH?)\n");
            s_record_path = NULL;
            return;
        }
    }
    static uint8_t blank[SNES_RENDER_WIDTH * 4];
    for (int y = 0; y < s_rec_h; y++)
        fwrite(y < rh ? src + (size_t)y * SNES_RENDER_WIDTH * 4 : blank, 1, (size_t)s_rec_w * 4, s_record_pipe);
}

bool platform_init(const char *window_title, int scale) {
    if (scale < 1) scale = 1;
    if (scale > 8) scale = 8;

    int win_w = SNES_RENDER_WIDTH * scale / 2;
    int win_h = SNES_RENDER_HEIGHT * scale / 2;

    if (s_headless) {
        if (SDL_Init(SDL_INIT_TIMER | SDL_INIT_EVENTS) != 0) {
            fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
            return false;
        }
        menu_overlay_init(NULL, NULL);   /* disables the menu */
        printf("platform: headless%s%s\n",s_record_path ? ", recording to " : "",
               s_record_path ? s_record_path : "");
        return true;
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }

#ifdef SNESRECOMP_HAS_TTF
    if (TTF_Init() == 0) {
        /* Try common Windows system font paths */
        const char *font_paths[] = {
            "C:/Windows/Fonts/segoeui.ttf",
            "C:/Windows/Fonts/arial.ttf",
            "C:/Windows/Fonts/consola.ttf",
            NULL
        };
        for (int i = 0; font_paths[i] && !s_font; i++) {
            s_font = TTF_OpenFont(font_paths[i], 14);
        }
    }
#endif

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
    want.freq = 32040;
    want.format = AUDIO_S16;
    want.channels = 2;
    want.samples = 1024;
    want.callback = NULL;

    s_audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (s_audio_dev > 0) {
        SDL_PauseAudioDevice(s_audio_dev, 0);
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
    if (s_headless && framebuffer) { headless_capture(framebuffer); return; }
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

    /* Clear to the menu-bar colour so the strip above the game (which the ImGui
     * menu bar doesn't fully paint) blends with the menu instead of showing
     * black. Falls back to black when the menu is disabled. */
    int br = 0, bg = 0, bb = 0;
    if (menu_h > 0) menu_overlay_get_bar_color(&br, &bg, &bb);
    SDL_SetRenderDrawColor(s_renderer, (Uint8)br, (Uint8)bg, (Uint8)bb, 255);
    SDL_RenderClear(s_renderer);
    SDL_RenderCopy(s_renderer, s_texture, &src, &dst);
    menu_overlay_render(s_renderer);   /* ImGui menu on top (no-op when disabled) */
    draw_toast();                      /* screenshot toast above everything */
    SDL_SetRenderDrawColor(s_renderer, 0, 0, 0, 255);   /* restore */
    SDL_RenderPresent(s_renderer);

    /* Take screenshot after present */
    if (s_screenshot_requested) {
        s_screenshot_requested = false;
        take_screenshot();
    }
}

void platform_queue_audio(const int16_t *samples, int sample_count) {
    if (s_audio_dev > 0 && samples && sample_count > 0) {
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
            if (!consumed) {
                /* Esc quits only when the menu isn't using it (rebind-cancel). */
                if (ev.key.keysym.sym == SDLK_ESCAPE) return false;
                /* Save-state hotkeys (mirror File -> Save / Load). */
                if (ev.key.keysym.sym == SDLK_F5) snesrecomp_save_state("smk_state.sav");
                if (ev.key.keysym.sym == SDLK_F8) snesrecomp_load_state("smk_state.sav");
                if (ev.key.keysym.sym == SDLK_F12 && !ev.key.repeat)
                    s_screenshot_requested = true;
            }
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
    if (s_headless) return;   /* run as fast as the host allows */
    uint64_t now = SDL_GetPerformanceCounter();
    double elapsed_ms = (double)(now - s_frame_start) * 1000.0
                        / (double)SDL_GetPerformanceFrequency();
    double remaining = FRAME_TIME_MS - elapsed_ms;
    if (remaining > 1.0) {
        SDL_Delay((uint32_t)(remaining - 0.5));
    }
    s_frame_start = SDL_GetPerformanceCounter();
}

void platform_request_screenshot(void) {
    s_screenshot_requested = true;
}

bool platform_mouse_to_snes(int *x, int *y) {
    /* A scripted mouse IS the pointer; the real cursor is irrelevant here (and
     * would make headless runs depend on where the user happened to leave it). */
    if (recomp_input_mouse_script_active()) return recomp_input_mouse_script_pointer(x, y);
    if (!s_renderer || !s_window) return false;

    int mx, my;
    SDL_GetMouseState(&mx, &my);

    /* SDL_GetMouseState is in window coordinates; the destination rect below is
     * in renderer output coordinates. They differ on HiDPI displays. */
    int win_w = 0, win_h = 0, out_w = 0, out_h = 0;
    SDL_GetWindowSize(s_window, &win_w, &win_h);
    SDL_GetRendererOutputSize(s_renderer, &out_w, &out_h);
    if (win_w <= 0 || win_h <= 0 || out_w <= 0 || out_h <= 0) return false;
    mx = mx * out_w / win_w;
    my = my * out_h / win_h;

    /* Same destination rect present_frame draws the game into. */
    int menu_h = menu_overlay_get_menubar_height();
    int dst_h = out_h - menu_h;
    if (dst_h <= 0) return false;
    if (mx < 0 || mx >= out_w || my < menu_h || my >= out_h) return false;

    /* The source rect is the active picture at 2x (512x448 for 224 NTSC lines),
     * so halving gives SNES pixels. */
    int rx, ry, rw, rh;
    snesrecomp_active_video_rect(&rx, &ry, &rw, &rh);
    int snes_w = rw / 2, snes_h = rh / 2;
    if (snes_w <= 0 || snes_h <= 0) return false;

    int sx = mx * snes_w / out_w;
    int sy = (my - menu_h) * snes_h / dst_h;
    if (sx < 0) sx = 0; else if (sx >= snes_w) sx = snes_w - 1;
    if (sy < 0) sy = 0; else if (sy >= snes_h) sy = snes_h - 1;

    *x = sx;
    *y = sy;
    return true;
}

void platform_shutdown(void) {
    if (s_record_pipe) {
#ifdef _WIN32
        _pclose(s_record_pipe);
#else
        pclose(s_record_pipe);
#endif
        s_record_pipe = NULL;
    }
    menu_overlay_shutdown();

    if (s_toast_tex) SDL_DestroyTexture(s_toast_tex);
    s_toast_tex = NULL;

#ifdef SNESRECOMP_HAS_TTF
    if (s_font) TTF_CloseFont(s_font);
    s_font = NULL;
    TTF_Quit();
#endif

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
