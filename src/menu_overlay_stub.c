/*
 * menu_overlay_stub.c — no-op implementation used when SNESRECOMP_MENU is OFF
 * (e.g. consumers that don't want the ImGui dependency). Keeps platform_sdl.c
 * and input.c linking with sensible defaults (no overlay, default bindings).
 */
#include "snesrecomp/menu_overlay.h"
#include "snesrecomp/input.h"

void  menu_overlay_init(struct SDL_Window *w, struct SDL_Renderer *r) { (void)w; (void)r; }
void  menu_overlay_shutdown(void) {}
int   menu_overlay_process_event(const void *e) { (void)e; return 0; }
void  menu_overlay_render(struct SDL_Renderer *r) { (void)r; }
int   menu_overlay_is_active(void) { return 0; }
int   menu_overlay_quit_requested(void) { return 0; }
int   menu_overlay_get_scale(void) { return 3; }
int   menu_overlay_get_vsync(void) { return 1; }
int   menu_overlay_get_filter(void) { return 1; }
int   menu_overlay_get_scanlines(void) { return 0; }
int   menu_overlay_get_show_fps(void) { return 0; }
float menu_overlay_get_volume(void) { return 1.0f; }

/* Default keyboard bindings (P1 snes9x-style; P2 right-hand cluster). */
int menu_overlay_key_for_button(int player, int idx) {
    /* SDL scancodes — avoid including SDL here; values are stable ABI. */
    static const int p1[12] = { 29,27,229,40,82,81,80,79,4,22,20,26 };  /* B,Y,Sel,Start,U,D,L,R,A,X,L,R */
    static const int p2[12] = { 14,13,228,88,96,93,92,94,15,12,24,18 };
    if (idx < 0 || idx >= 12) return -1;
    if (player == 1) return p1[idx];
    if (player == 2) return p2[idx];
    return -1;
}
int menu_overlay_pad_button_for_button(int player, int idx) {
    /* SDL_GameControllerButton mapping: B=A,A=B,Y=X,X=Y,L=LB,R=RB,Sel=Back,
     * Start=Start,dpad U/D/L/R. */
    static const int pad[12] = { 0,2,4,6,11,12,13,14,1,3,9,10 };
    (void)player;
    if (idx < 0 || idx >= 12) return -1;
    return pad[idx];
}
