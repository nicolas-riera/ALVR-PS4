#pragma once

#include <stdint.h>

#include "tracker.h"

// PS Move controllers (libSceMove), registered with the VR tracker once connected.
// The tracker drives the sphere colour itself.

#define MOVE_MAX 2

// Button bits of MoveData.buttons, mapped on hardware from logged presses.
enum MoveButton : uint16_t {
    MOVE_BUTTON_SELECT = 0x0001, // used by the system for screenshots: never bind it
    MOVE_BUTTON_T = 0x0002,      // trigger touched (set from ~16 % travel; analog value in MoveController.trigger)
    MOVE_BUTTON_MOVE = 0x0004,
    MOVE_BUTTON_START = 0x0008,
    MOVE_BUTTON_TRIANGLE = 0x0010,
    MOVE_BUTTON_CIRCLE = 0x0020,
    MOVE_BUTTON_CROSS = 0x0040,
    MOVE_BUTTON_SQUARE = 0x0080,
    MOVE_BUTTON_PS = 0x8000,     // intercepted by the system
};

struct MoveController {
    int handle;       // sceMoveOpen handle, < 0 if not opened
    bool connected;   // sceMoveGetDeviceInfo / ReadState succeed
    float sphere_radius;
    uint16_t buttons; // MoveButton bits
    uint16_t trigger; // 0..255
    uint64_t next_register_us; // tracker registration retry time
    uint8_t vibration;         // current motor intensity
    uint64_t vibration_end_us; // 0 = no timed pulse
    // Battery byte of the raw PS Move report: 0..5 (5 = full), 0xEE charging, 0xEF charged
    // (on USB), -1 unknown. Read every 5 s (libSceMove itself discards it).
    int battery_raw;
    uint64_t next_battery_us;
    TrackedDevice track;
};

// Battery as a 0..1 gauge and charging flag; false when unknown.
bool move_battery(const MoveController &ctl, float *gauge, bool *charging);

// Opens both controllers for the user and registers them with the tracker
// (call after tracker_start).
void move_start(int move_module, int user_id, MoveController out[MOVE_MAX]);
// Reads buttons and tracking; (un)registers controllers as they (dis)connect.
void move_update(MoveController ctl[MOVE_MAX]);
// Rumble: intensity 0..255, for duration_ms (0 = until changed).
void move_vibrate(MoveController *ctl, uint8_t intensity, uint32_t duration_ms);
// RGB of a tracker LED colour index.
uint32_t move_led_rgb(uint32_t led_color);
