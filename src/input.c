/*
 * Input adapter — maps keyboard/gamepad/mouse to SNES input state
 * and feeds it into LakeSnes's input system.
 *
 * Supports both standard SNES joypad and SNES Mouse devices.
 */

#include "snesrecomp/input.h"
#include "snes.h"
#include <SDL.h>
#include <string.h>

/* Access to the LakeSnes instance */
extern Snes *snesrecomp_get_snes(void);

/* Per-port device type and mouse state */
static int s_device_type[2] = { SNES_INPUT_JOYPAD, SNES_INPUT_JOYPAD };
static SnesMouseState s_mouse[2];
static SnesSuperScopeState s_scope;

/* Accumulated mouse motion (reset each frame after latching) */
static int s_mouse_accum_dx = 0;
static int s_mouse_accum_dy = 0;

void recomp_input_set_device(int port, int device_type) {
    if (port < 1 || port > 2) return;
    s_device_type[port - 1] = device_type;
    if (device_type == SNES_INPUT_MOUSE) {
        memset(&s_mouse[port - 1], 0, sizeof(SnesMouseState));
        s_mouse[port - 1].speed = 2; /* default to fast sensitivity */
    }
    if (device_type == SNES_INPUT_SUPERSCOPE && port == 2) {
        memset(&s_scope, 0, sizeof(s_scope));
        s_scope.offscreen = true;
    }
    /* Update LakeSnes device type */
    Snes *snes = snesrecomp_get_snes();
    if (snes) {
        int lk_type = 1; /* inputDeviceController */
        if (device_type == SNES_INPUT_MOUSE) lk_type = 2;
        if (device_type == SNES_INPUT_SUPERSCOPE) lk_type = 3;
        snes_setInputDevice(snes, port, lk_type);
    }
}

SnesMouseState *recomp_input_get_mouse(int port) {
    if (port < 1 || port > 2) return NULL;
    if (s_device_type[port - 1] != SNES_INPUT_MOUSE) return NULL;
    return &s_mouse[port - 1];
}

SnesSuperScopeState *recomp_input_get_scope(void) {
    if (s_device_type[1] != SNES_INPUT_SUPERSCOPE) return NULL;
    return &s_scope;
}

void recomp_input_set_scope(uint16_t x, uint16_t y, uint8_t buttons) {
    s_scope.x = x;
    s_scope.y = y;
    s_scope.buttons = buttons;
    s_scope.offscreen = (x >= 256 || y >= 224);
}

/* Accumulate mouse motion from SDL events (called from platform layer) */
void recomp_input_accumulate_mouse(int dx, int dy) {
    s_mouse_accum_dx += dx;
    s_mouse_accum_dy += dy;
}

/*
 * Build the 32-bit SNES mouse serial data word.
 *
 * Format (bit 0 = first read):
 *   Bits  0-7:  0x00 (mouse signature)
 *   Bit   8:    Right button
 *   Bit   9:    Left button
 *   Bits 10-11: Speed (sensitivity)
 *   Bits 12-15: 0
 *   Bits 16-23: Y displacement (bit 16=sign, 17-23=magnitude, max 127)
 *   Bits 24-31: X displacement (bit 24=sign, 25-31=magnitude, max 127)
 */
static uint32_t build_mouse_serial(const SnesMouseState *ms) {
    uint32_t data = 0;

    /* Bits 0-7: signature (all zero = mouse) */
    /* Already zero */

    /* Bit 8: right button, Bit 9: left button */
    if (ms->right) data |= (1 << 8);
    if (ms->left)  data |= (1 << 9);

    /* Bits 10-11: speed */
    data |= ((uint32_t)(ms->speed & 3) << 10);

    /* Bits 12-15: zero */

    /* Bits 16-23: Y displacement */
    int dy = ms->dy;
    if (dy < -127) dy = -127;
    if (dy > 127) dy = 127;
    if (dy < 0) {
        data |= (1 << 16);               /* sign bit */
        data |= ((uint32_t)(-dy) << 17); /* magnitude */
    } else {
        data |= ((uint32_t)dy << 17);
    }

    /* Bits 24-31: X displacement */
    int dx = ms->dx;
    if (dx < -127) dx = -127;
    if (dx > 127) dx = 127;
    if (dx < 0) {
        data |= (1 << 24);
        data |= ((uint32_t)(-dx) << 25);
    } else {
        data |= ((uint32_t)dx << 25);
    }

    return data;
}

void recomp_input_update(void) {
    Snes *snes = snesrecomp_get_snes();
    if (!snes) return;

    const uint8_t *keys = SDL_GetKeyboardState(NULL);
    uint32_t mouse_buttons = SDL_GetMouseState(NULL, NULL);

    /* --- Port 1 --- */
    if (s_device_type[0] == SNES_INPUT_MOUSE) {
        SnesMouseState *ms = &s_mouse[0];

        /* Capture accumulated mouse delta */
        ms->dx = s_mouse_accum_dx;
        ms->dy = s_mouse_accum_dy;
        s_mouse_accum_dx = 0;
        s_mouse_accum_dy = 0;

        /* SDL mouse buttons */
        ms->left  = (mouse_buttons & SDL_BUTTON_LMASK) != 0;
        ms->right = (mouse_buttons & SDL_BUTTON_RMASK) != 0;

        /* Build serial data and latch it */
        ms->latched = build_mouse_serial(ms);
        ms->read_count = 0;

        /*
         * For auto-joypad compatibility: set the first 16 bits of the
         * mouse serial data as the controller state. The auto-joypad
         * read will capture these (signature + buttons + speed).
         */
        snes->input1->currentState = (uint16_t)(ms->latched & 0xFFFF);
    } else {
        /* Standard joypad */
        snes_setButtonState(snes, 1, SNES_BTN_IDX_UP,     keys[SDL_SCANCODE_UP]);
        snes_setButtonState(snes, 1, SNES_BTN_IDX_DOWN,   keys[SDL_SCANCODE_DOWN]);
        snes_setButtonState(snes, 1, SNES_BTN_IDX_LEFT,   keys[SDL_SCANCODE_LEFT]);
        snes_setButtonState(snes, 1, SNES_BTN_IDX_RIGHT,  keys[SDL_SCANCODE_RIGHT]);
        snes_setButtonState(snes, 1, SNES_BTN_IDX_B,      keys[SDL_SCANCODE_Z]);
        snes_setButtonState(snes, 1, SNES_BTN_IDX_Y,      keys[SDL_SCANCODE_X]);
        snes_setButtonState(snes, 1, SNES_BTN_IDX_A,      keys[SDL_SCANCODE_A]);
        snes_setButtonState(snes, 1, SNES_BTN_IDX_X,      keys[SDL_SCANCODE_S]);
        snes_setButtonState(snes, 1, SNES_BTN_IDX_L,      keys[SDL_SCANCODE_Q]);
        snes_setButtonState(snes, 1, SNES_BTN_IDX_R,      keys[SDL_SCANCODE_W]);
        snes_setButtonState(snes, 1, SNES_BTN_IDX_START,  keys[SDL_SCANCODE_RETURN]);
        snes_setButtonState(snes, 1, SNES_BTN_IDX_SELECT, keys[SDL_SCANCODE_RSHIFT]);
    }

    /* --- Port 2 --- */
    if (s_device_type[1] == SNES_INPUT_SUPERSCOPE) {
        /* Super Scope: feed position and buttons into LakeSnes */
        snes_setSuperScopeState(snes, s_scope.x, s_scope.y, s_scope.buttons);
    }
    /* (Port 2 joypad is handled by direct snes_setButtonState calls from recomp code) */
}

uint16_t recomp_input_read_joypad(int port) {
    Snes *snes = snesrecomp_get_snes();
    if (!snes || port < 0 || port > 3) return 0;
    return snes->portAutoRead[port];
}

uint8_t recomp_input_serial_read(int port) {
    if (port < 1 || port > 2) return 0;
    int idx = port - 1;

    if (s_device_type[idx] == SNES_INPUT_MOUSE) {
        SnesMouseState *ms = &s_mouse[idx];

        /* Return bits from the 32-bit latched data, MSB first */
        if (ms->read_count >= 32) return 1; /* open bus after 32 bits */

        int bit_pos = 31 - ms->read_count;
        uint8_t ret = (ms->latched >> bit_pos) & 1;
        ms->read_count++;
        return ret;
    }

    /* For joypad and Super Scope, delegate to LakeSnes (handles serial protocol) */
    Snes *snes = snesrecomp_get_snes();
    if (!snes) return 0;
    if (port == 1) return input_read(snes->input1);
    return input_read(snes->input2);
}

void recomp_input_latch(bool value) {
    Snes *snes = snesrecomp_get_snes();
    if (!snes) return;

    /* Latch LakeSnes controllers */
    input_latch(snes->input1, value);
    input_latch(snes->input2, value);

    /* Reset mouse serial read position on latch */
    if (!value) { /* falling edge */
        for (int i = 0; i < 2; i++) {
            if (s_device_type[i] == SNES_INPUT_MOUSE) {
                s_mouse[i].read_count = 0;
                s_mouse[i].latched = build_mouse_serial(&s_mouse[i]);
            }
        }
    }
}
