/*
 * Input adapter — maps keyboard/gamepad/mouse to SNES input state
 * and feeds it into LakeSnes's input system.
 *
 * Supports both standard SNES joypad and SNES Mouse devices.
 */

#include "snesrecomp/input.h"
#include "snesrecomp/menu_overlay.h"
#include "snesrecomp/platform.h"
#include "snes.h"
#include <SDL.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* Up to two opened game controllers (Xbox-style), assigned to players 1 and 2.
 * Empty slots are filled lazily each frame so hotplugged pads Just Work. */
static SDL_GameController *s_gc[2] = { NULL, NULL };
#define GC_AXIS_DEADZONE 12000

static void ensure_gamepads(void) {
    for (int slot = 0; slot < 2; slot++) {
        if (s_gc[slot]) continue;
        for (int i = 0; i < SDL_NumJoysticks(); i++) {
            if (!SDL_IsGameController(i)) continue;
            SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(i);
            int held = 0;
            for (int s2 = 0; s2 < 2; s2++) {
                if (!s_gc[s2]) continue;
                SDL_Joystick *j = SDL_GameControllerGetJoystick(s_gc[s2]);
                if (j && SDL_JoystickInstanceID(j) == id) { held = 1; break; }
            }
            if (held) continue;
            s_gc[slot] = SDL_GameControllerOpen(i);
            if (s_gc[slot]) break;
        }
    }
}

/* True if `player` (1/2) currently presses SNES button `btn` via its bound key
 * or gamepad button, plus left-stick-as-dpad for the directions. */
static bool button_pressed(int player, int btn, const uint8_t *keys) {
    int sc = menu_overlay_key_for_button(player, btn);
    if (sc >= 0 && keys[sc]) return true;

    SDL_GameController *gc = s_gc[player - 1];
    if (gc) {
        int pb = menu_overlay_pad_button_for_button(player, btn);
        if (pb >= 0 && SDL_GameControllerGetButton(gc, (SDL_GameControllerButton)pb)) return true;
        /* Left analog stick also drives the d-pad directions. */
        int ax = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX);
        int ay = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY);
        if (btn == SNES_BTN_IDX_LEFT  && ax < -GC_AXIS_DEADZONE) return true;
        if (btn == SNES_BTN_IDX_RIGHT && ax >  GC_AXIS_DEADZONE) return true;
        if (btn == SNES_BTN_IDX_UP    && ay < -GC_AXIS_DEADZONE) return true;
        if (btn == SNES_BTN_IDX_DOWN  && ay >  GC_AXIS_DEADZONE) return true;
    }
    return false;
}

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

/*
 * Scripted mouse input — drives the SNES Mouse from an env var so a headless
 * run can navigate menus reproducibly (regression captures, reaching a screen
 * the recomp can't drive yet, bisecting a visual bug against real-frame mode).
 *
 *   SNESRECOMP_MOUSE_SCRIPT="frame:dx:dy:btn[:count] ..."
 *
 *   frame  1-based frame number (counts recomp_input_update calls)
 *   dx,dy  signed per-frame mouse delta, SNES pixels
 *   btn    0 (none), L, R, or LR
 *   count  optional; repeat this step for N consecutive frames (default 1)
 *
 * Example — move right 8px/frame for 20 frames from frame 200, then click:
 *   SNESRECOMP_MOUSE_SCRIPT="200:8:0:0:20 240:0:0:L:3 243:0:0:0:2"
 *
 * Steps are whitespace-separated and may be listed in any order. A frame with
 * no step leaves the live SDL mouse untouched.
 *
 * Absolute pointer: "frame:@x:@y:btn[:count]" parks a virtual host pointer at
 * SNES pixel (x, y) instead of sending a delta. platform_mouse_to_snes() reports
 * it, so a game that steers its cursor toward the host pointer (closed loop)
 * walks straight there; the pointer stays put until the next @ step.
 *   SNESRECOMP_MOUSE_SCRIPT="600:@40:@212:0:20 620:@40:@212:L:2"
 */
#define MOUSE_SCRIPT_MAX 4096
typedef struct { int frame, dx, dy, count; bool left, right, abs; } MouseStep;
static MouseStep s_msteps[MOUSE_SCRIPT_MAX];
static int       s_mstep_count = 0;
static int       s_mscript_frame = 0;
static int       s_mscript_parsed = 0;
static bool      s_mptr_valid = false;
static int       s_cursor_x_addr = -1, s_cursor_y_addr = -1;
static int       s_mptr_x = 0, s_mptr_y = 0;

static void mouse_script_parse(void) {
    s_mscript_parsed = 1;
    const char *p = getenv("SNESRECOMP_MOUSE_SCRIPT");
    if (!p) return;
    while (*p && s_mstep_count < MOUSE_SCRIPT_MAX) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        if (!*p) break;
        char *end;
        long frame = strtol(p, &end, 10);
        if (end == p || *end != ':') break;
        bool abs = end[1] == '@';
        long dx = strtol(end + 1 + abs, &end, 10);
        if (*end != ':') break;
        long dy = strtol(end + 1 + (end[1] == '@'), &end, 10);
        if (*end != ':') break;
        p = end + 1;
        bool l = false, r = false;
        while (*p == 'L' || *p == 'l' || *p == 'R' || *p == 'r' || *p == '0') {
            if (*p == 'L' || *p == 'l') l = true;
            if (*p == 'R' || *p == 'r') r = true;
            p++;
        }
        long count = 1;
        if (*p == ':') count = strtol(p + 1, &end, 10), p = end;
        if (count < 1) count = 1;
        MouseStep *s = &s_msteps[s_mstep_count++];
        s->frame = (int)frame; s->dx = (int)dx; s->dy = (int)dy;
        s->count = (int)count; s->left = l; s->right = r; s->abs = abs;
    }
    fprintf(stderr, "snesrecomp: mouse script parsed, %d step(s)\n", s_mstep_count);
}

/*
 * Lock the game's cursor to the host pointer (live, or a scripted "@x:@y").
 * The SNES mouse is relative, so forwarded host deltas drift: slow moves
 * truncate to zero and anything the game rejects at its cursor bounds is lost
 * for good. Instead, each frame report the displacement that moves the game's
 * cursor (at the registered WRAM address) toward the pointer, recomputed from
 * scratch so error cannot accumulate.
 *
 * The game may apply a reported delta in the same frame or the next one (real-
 * frame mode latches at the following auto-read), so a delta sent every frame
 * overshoots and rings, by an amount that depends on timing. Instead: send the
 * whole error, then one still frame, then measure again. That lands exactly
 * whatever the lag, and stays deterministic, which the conformance harness
 * needs. Pointer off the game area: fall back to raw deltas.
 */
static void steer_cursor(SnesMouseState *ms) {
    static bool s_settling = false;
    Snes *snes = snesrecomp_get_snes();
    int px, py;
    if (s_cursor_x_addr < 0 || !snes || !platform_mouse_to_snes(&px, &py)) return;
    if (s_settling) { s_settling = false; ms->dx = ms->dy = 0; return; }
    ms->dx = px - (int16_t)(snes->ram[s_cursor_x_addr] | snes->ram[s_cursor_x_addr + 1] << 8);
    ms->dy = py - (int16_t)(snes->ram[s_cursor_y_addr] | snes->ram[s_cursor_y_addr + 1] << 8);
    s_settling = ms->dx || ms->dy;
}

void recomp_input_set_mouse_cursor_addr(int x_addr, int y_addr) {
    s_cursor_x_addr = x_addr & 0x1FFFF;
    s_cursor_y_addr = y_addr & 0x1FFFF;
}

bool recomp_input_mouse_script_pointer(int *x, int *y) {
    if (!s_mptr_valid) return false;
    *x = s_mptr_x; *y = s_mptr_y;
    return true;
}

bool recomp_input_mouse_script_active(void) {
    if (!s_mscript_parsed) mouse_script_parse();
    return s_mstep_count > 0;
}

static void mouse_script_apply(SnesMouseState *ms) {
    if (!s_mscript_parsed) mouse_script_parse();
    if (!s_mstep_count) return;
    s_mscript_frame++;
    for (int i = 0; i < s_mstep_count; i++) {
        const MouseStep *s = &s_msteps[i];
        if (s_mscript_frame >= s->frame && s_mscript_frame < s->frame + s->count) {
            if (s->abs) {
                s_mptr_valid = true; s_mptr_x = s->dx; s_mptr_y = s->dy;
                ms->dx = 0; ms->dy = 0;   /* steer_cursor() supplies the motion */
            } else {
                ms->dx = s->dx;
                ms->dy = s->dy;
            }
            ms->left = s->left;
            ms->right = s->right;
            return;
        }
    }
    /* No step covers this frame: hold still rather than inherit the real cursor. */
    ms->dx = 0; ms->dy = 0; ms->left = false; ms->right = false;
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

        /* A scripted step for this frame overrides the live SDL mouse, so a
         * headless run is reproducible and doesn't fight the real cursor. */
        mouse_script_apply(ms);
        steer_cursor(ms);

        /* Build serial data and latch it (used by the recomp path's own
         * $4016/$4017 reads). */
        ms->latched = build_mouse_serial(ms);
        ms->read_count = 0;

        /* Also feed LakeSnes's own mouse device. The recomp path reads the
         * serial word above, but real-frame mode runs the genuine CPU against
         * LakeSnes's Input struct — without this the mouse is dead there. */
        snes_setMouseState(snes, 1, (int16_t)ms->dx, (int16_t)ms->dy,
                           ms->left, ms->right);

        if (getenv("SNESRECOMP_MOUSE_DEBUG")) {
            static int n = 0;
            if (n++ < 200 && (ms->dx || ms->dy || ms->left || ms->right))
                fprintf(stderr, "[MOUSE] dx=%d dy=%d L=%d R=%d packet=%08X\n",
                        ms->dx, ms->dy, ms->left, ms->right,
                        snes->input1->mouseLatchedData);
        }

        /*
         * For auto-joypad compatibility: set the first 16 bits of the
         * mouse serial data as the controller state. The auto-joypad
         * read will capture these (signature + buttons + speed).
         */
        snes->input1->currentState = (uint16_t)(ms->latched & 0xFFFF);
    } else {
        /* Standard joypad — driven from the configurable keyboard + gamepad
         * bindings (see menu_overlay). When the menu has input focus, the game
         * receives no presses. */
        ensure_gamepads();
        int block = menu_overlay_is_active();
        for (int btn = 0; btn < 12; btn++)
            snes_setButtonState(snes, 1, btn, block ? false : button_pressed(1, btn, keys));
    }

    /* --- Port 2 --- */
    if (s_device_type[1] == SNES_INPUT_SUPERSCOPE) {
        /* Super Scope: feed position and buttons into LakeSnes */
        snes_setSuperScopeState(snes, s_scope.x, s_scope.y, s_scope.buttons);
    } else if (s_device_type[1] == SNES_INPUT_JOYPAD) {
        /* Player 2 joypad — second keyboard set + second gamepad. */
        ensure_gamepads();
        int block = menu_overlay_is_active();
        for (int btn = 0; btn < 12; btn++)
            snes_setButtonState(snes, 2, btn, block ? false : button_pressed(2, btn, keys));
    }
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
