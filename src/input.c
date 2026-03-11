/*
 * Input adapter — maps keyboard/gamepad to SNES button state and
 * feeds it into LakeSnes's input system.
 */

#include "snesrecomp/input.h"
#include "snes.h"
#include <SDL.h>

/* Access to the LakeSnes instance */
extern Snes *snesrecomp_get_snes(void);

void recomp_input_update(void) {
    Snes *snes = snesrecomp_get_snes();
    if (!snes) return;

    const uint8_t *keys = SDL_GetKeyboardState(NULL);

    /* D-pad */
    snes_setButtonState(snes, 1, SNES_BTN_IDX_UP,     keys[SDL_SCANCODE_UP]);
    snes_setButtonState(snes, 1, SNES_BTN_IDX_DOWN,   keys[SDL_SCANCODE_DOWN]);
    snes_setButtonState(snes, 1, SNES_BTN_IDX_LEFT,   keys[SDL_SCANCODE_LEFT]);
    snes_setButtonState(snes, 1, SNES_BTN_IDX_RIGHT,  keys[SDL_SCANCODE_RIGHT]);

    /* Face buttons: Z=B, X=Y, A=A, S=X */
    snes_setButtonState(snes, 1, SNES_BTN_IDX_B,      keys[SDL_SCANCODE_Z]);
    snes_setButtonState(snes, 1, SNES_BTN_IDX_Y,      keys[SDL_SCANCODE_X]);
    snes_setButtonState(snes, 1, SNES_BTN_IDX_A,      keys[SDL_SCANCODE_A]);
    snes_setButtonState(snes, 1, SNES_BTN_IDX_X,      keys[SDL_SCANCODE_S]);

    /* Shoulder buttons */
    snes_setButtonState(snes, 1, SNES_BTN_IDX_L,      keys[SDL_SCANCODE_Q]);
    snes_setButtonState(snes, 1, SNES_BTN_IDX_R,      keys[SDL_SCANCODE_W]);

    /* Start / Select */
    snes_setButtonState(snes, 1, SNES_BTN_IDX_START,  keys[SDL_SCANCODE_RETURN]);
    snes_setButtonState(snes, 1, SNES_BTN_IDX_SELECT, keys[SDL_SCANCODE_RSHIFT]);
}

uint16_t recomp_input_read_joypad(int port) {
    Snes *snes = snesrecomp_get_snes();
    if (!snes || port < 0 || port > 3) return 0;
    return snes->portAutoRead[port];
}
