/*
 * menu_overlay — Dear ImGui menu system for the SMK launcher.
 *
 * Persistent main menu bar (File / Graphics / Sound / Controller / Multiplayer /
 * Help) over the game, modelled on gb-recompiled's menu_gui. Supports keyboard
 * AND gamepad (Xbox-style SDL_GameController) bindings for BOTH players.
 *
 * The C++ implementation (menu_overlay.cpp) exposes this C API so the C SDL
 * platform + input layers can drive it. Disabled automatically in
 * headless/scripted runs (SMK_HEADLESS) so it never adds overhead or
 * intercepts input during automated flows.
 */
#ifndef SNESRECOMP_MENU_OVERLAY_H
#define SNESRECOMP_MENU_OVERLAY_H

#ifdef __cplusplus
extern "C" {
#endif

struct SDL_Window;
struct SDL_Renderer;

/* Lifecycle. init() is a no-op (stays disabled) when SMK_HEADLESS/SMK_SCRIPT is
 * set. All other calls are safe when disabled. */
void menu_overlay_init(struct SDL_Window *window, struct SDL_Renderer *renderer);
void menu_overlay_shutdown(void);

/* Feed an SDL_Event (void* to keep this header SDL-free).
 * Returns 1 if the menu captured the event (game should ignore it). */
int  menu_overlay_process_event(const void *sdl_event);

/* Build + render the menu for this frame (after the game frame is copied to the
 * renderer, before SDL_RenderPresent). */
void menu_overlay_render(struct SDL_Renderer *renderer);

/* 1 while the menu is capturing input — the game must ignore keyboard/pad. */
int  menu_overlay_is_active(void);

/* Height in pixels of the main menu bar (0 when the menu is disabled). The
 * platform renders the game below this so the menu never covers the HUD. */
int  menu_overlay_get_menubar_height(void);

/* Set by File -> Quit. */
int  menu_overlay_quit_requested(void);

/* ---- Settings accessors (polled by the platform each frame) ---- */
int   menu_overlay_get_scale(void);
int   menu_overlay_get_vsync(void);
int   menu_overlay_get_filter(void);      /* 0=nearest, 1=linear */
int   menu_overlay_get_scanlines(void);
int   menu_overlay_get_show_fps(void);
float menu_overlay_get_volume(void);      /* 0.0..1.0 (already 0 when muted) */

/* ---- Per-player input bindings (player = 1 or 2) ---- */
/* Keyboard SDL_Scancode bound to a SNES button (SNES_BTN_IDX_*), or -1. */
int menu_overlay_key_for_button(int player, int snes_btn_idx);
/* Gamepad SDL_GameControllerButton bound to a SNES button, or -1. */
int menu_overlay_pad_button_for_button(int player, int snes_btn_idx);

#ifdef __cplusplus
}
#endif

#endif /* SNESRECOMP_MENU_OVERLAY_H */
