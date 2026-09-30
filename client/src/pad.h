#pragma once

#include <stdint.h>

#include "tracker.h"

// DualShock 4 of the user who started the app: read with libScePad, tracked by its light
// bar through libSceVrTracker (like the PS Moves). Shown in the lobby only; nothing goes
// to SteamVR.

enum PadButton : uint32_t {
    PAD_BUTTON_L3 = 0x0002,
    PAD_BUTTON_R3 = 0x0004,
    PAD_BUTTON_OPTIONS = 0x0008,
    PAD_BUTTON_UP = 0x0010,
    PAD_BUTTON_RIGHT = 0x0020,
    PAD_BUTTON_DOWN = 0x0040,
    PAD_BUTTON_LEFT = 0x0080,
    PAD_BUTTON_L2 = 0x0100,
    PAD_BUTTON_R2 = 0x0200,
    PAD_BUTTON_L1 = 0x0400,
    PAD_BUTTON_R1 = 0x0800,
    PAD_BUTTON_TRIANGLE = 0x1000,
    PAD_BUTTON_CIRCLE = 0x2000,
    PAD_BUTTON_CROSS = 0x4000,
    PAD_BUTTON_SQUARE = 0x8000,
    PAD_BUTTON_TOUCH_PAD = 0x100000,
};

struct PadTouch {
    bool down;
    float x, y; // 0..1 across the touchpad, from the top left corner
};

struct PadController {
    int handle;     // scePadOpen handle, -1 if none
    int owner;      // user id
    bool connected;
    bool was_tracked; // registered with the tracker on the previous update
    uint32_t buttons;
    float lx, ly, rx, ry; // sticks, -1..1 (+x right, +y up)
    float l2, r2;         // analog triggers, 0..1
    PadTouch touch[2];
    uint16_t touch_res_x, touch_res_y;
    int battery_raw;      // raw level (see pad_battery), -1 unknown
    uint64_t next_battery_us;
    uint8_t vib_large, vib_small;
    uint64_t vibration_end_us; // 0: no timed pulse
    TrackedDevice track;
    uint64_t next_register_us;
};

// Opens the user's DualShock 4 (once per logged-in user).
void pad_start(int user_id, PadController *pad);
void pad_update(PadController *pad);
// Motors: large (left grip, low rumble) and small (right grip, fast buzz), 0..255. With
// duration_ms > 0 both stop after it; 0 keeps them until the next call.
void pad_vibrate(PadController *pad, uint8_t large, uint8_t small, uint32_t duration_ms);
// Vibration strength setting, 0-100 % (0 = off), for every later pad_vibrate.
void pad_set_vibration_strength(int percent);
// Battery gauge 0..1 and charging state; false when unknown.
bool pad_battery(const PadController &pad, float *gauge, bool *charging);
