#ifndef SNESRECOMP_INPUT_H
#define SNESRECOMP_INPUT_H

/*
 * SNES joypad input — maps keyboard/gamepad to SNES button state.
 *
 * Button state is automatically fed into LakeSnes's input system
 * during snesrecomp_begin_frame(), so the hardware auto-joypad read
 * registers ($4218-$421F) work exactly like real hardware.
 */

#include <stdint.h>

/* SNES joypad button indices (for snes_setButtonState) */
#define SNES_BTN_IDX_B       0
#define SNES_BTN_IDX_Y       1
#define SNES_BTN_IDX_SELECT  2
#define SNES_BTN_IDX_START   3
#define SNES_BTN_IDX_UP      4
#define SNES_BTN_IDX_DOWN    5
#define SNES_BTN_IDX_LEFT    6
#define SNES_BTN_IDX_RIGHT   7
#define SNES_BTN_IDX_A       8
#define SNES_BTN_IDX_X       9
#define SNES_BTN_IDX_L       10
#define SNES_BTN_IDX_R       11

/* SNES joypad button bitmask values (for reading $4218-$421F) */
#define SNES_BTN_B       0x8000
#define SNES_BTN_Y       0x4000
#define SNES_BTN_SELECT  0x2000
#define SNES_BTN_START   0x1000
#define SNES_BTN_UP      0x0800
#define SNES_BTN_DOWN    0x0400
#define SNES_BTN_LEFT    0x0200
#define SNES_BTN_RIGHT   0x0100
#define SNES_BTN_A       0x0080
#define SNES_BTN_X       0x0040
#define SNES_BTN_L       0x0020
#define SNES_BTN_R       0x0010

/* Update keyboard/gamepad state and feed into LakeSnes input system */
void recomp_input_update(void);

/* Read joypad auto-read result for a port (0-3) */
uint16_t recomp_input_read_joypad(int port);

#endif /* SNESRECOMP_INPUT_H */
