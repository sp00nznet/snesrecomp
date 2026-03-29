#ifndef SNESRECOMP_INPUT_H
#define SNESRECOMP_INPUT_H

/*
 * SNES input — maps keyboard/gamepad/mouse to SNES controller state.
 *
 * Supports both standard joypad and SNES Mouse input devices.
 * Button state is automatically fed into LakeSnes's input system
 * during snesrecomp_begin_frame(), so the hardware auto-joypad read
 * registers ($4218-$421F) work exactly like real hardware.
 */

#include <stdint.h>
#include <stdbool.h>

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

/* Input device types */
#define SNES_INPUT_JOYPAD        0
#define SNES_INPUT_MOUSE         1
#define SNES_INPUT_SUPERSCOPE    2

/*
 * SNES Mouse state — tracks SDL mouse and converts to the SNES mouse
 * serial protocol format.
 *
 * SNES Mouse serial data (32 bits, read via $4016/$4017):
 *   Bits  0-7:  Signature (0x00 = mouse connected)
 *   Bit   8:    Right button
 *   Bit   9:    Left button
 *   Bits 10-11: Speed/sensitivity (0=slow, 1=medium, 2=fast)
 *   Bits 12-15: Unused (0)
 *   Bits 16-23: Y displacement (bit 16=sign, bits 17-23=magnitude)
 *   Bits 24-31: X displacement (bit 24=sign, bits 25-31=magnitude)
 *
 * Auto-joypad reads the first 16 bits into portAutoRead[0/1].
 * The displacement bytes (bits 16-31) must be read manually from
 * $4016/$4017 after auto-joypad completes.
 */
typedef struct {
    int     dx, dy;     /* accumulated mouse delta since last latch */
    bool    left;       /* left button pressed */
    bool    right;      /* right button pressed */
    uint8_t speed;      /* sensitivity: 0=slow, 1=medium, 2=fast */
    int     read_count; /* serial read position (for $4016/$4017 reads) */
    uint32_t latched;   /* full 32-bit latched serial data */
} SnesMouseState;

/*
 * Super Scope state — tracks aimed position and button state.
 *
 * The Super Scope connects to port 2 and uses:
 *   - Serial data: 16-bit button/status word via $4017 / auto-read $421A
 *   - PPU H/V counter latch: photodiode triggers counter capture
 *
 * Serial data format (auto-read $421A/$421B, bit 15 = first read):
 *   Bits 15-12: Noise (1 when on-screen)
 *   Bit 11:     Fire button
 *   Bit 10:     Cursor button
 *   Bit 9:      Turbo switch
 *   Bit 8:      Pause button
 *   Bit 7:      Offscreen flag
 *   Bits 6-0:   Signature (0)
 */
typedef struct {
    uint16_t x, y;      /* aimed position (0-255, 0-223) */
    uint8_t  buttons;   /* bit 0=fire, 1=cursor, 2=turbo, 3=pause */
    bool     offscreen; /* true when aiming outside visible area */
} SnesSuperScopeState;

/*
 * Set the input device type for a port (1 or 2).
 * Use SNES_INPUT_JOYPAD (default), SNES_INPUT_MOUSE, or SNES_INPUT_SUPERSCOPE.
 * Note: Super Scope always uses port 2 per SNES hardware design.
 */
void recomp_input_set_device(int port, int device_type);

/* Get the mouse state for a port (returns NULL if port is not a mouse) */
SnesMouseState *recomp_input_get_mouse(int port);

/* Get the Super Scope state (returns NULL if port 2 is not a Super Scope) */
SnesSuperScopeState *recomp_input_get_scope(void);

/*
 * Set Super Scope aim position and buttons directly.
 * Convenience function — equivalent to modifying the state from get_scope().
 */
void recomp_input_set_scope(uint16_t x, uint16_t y, uint8_t buttons);

/* Update keyboard/gamepad/mouse state and feed into LakeSnes input system */
void recomp_input_update(void);

/* Read joypad auto-read result for a port (0-3) */
uint16_t recomp_input_read_joypad(int port);

/*
 * Read the next serial bit from an input port (emulates $4016/$4017 reads).
 * For joypad: returns controller serial data.
 * For mouse: returns the full 32-bit mouse serial data bit-by-bit.
 * For Super Scope: returns 16-bit button/status data bit-by-bit.
 */
uint8_t recomp_input_serial_read(int port);

/* Latch input state (emulates write to $4016) */
void recomp_input_latch(bool value);

#endif /* SNESRECOMP_INPUT_H */
