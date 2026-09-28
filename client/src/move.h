#pragma once

#include <stdint.h>

#include "tracker.h"

// PS Move controllers (libSceMove), registered with the VR tracker once connected.
// The tracker drives the sphere colour itself.

#define MOVE_MAX 2

struct MoveController {
    int handle;       // sceMoveOpen handle, < 0 if not opened
    bool connected;   // sceMoveGetDeviceInfo / ReadState succeed
    float sphere_radius;
    uint16_t buttons; // raw button bits (mapping logged, see move.cpp)
    uint16_t trigger; // 0..255
    uint64_t next_register_us; // tracker registration retry time
    TrackedDevice track;
};

// Opens both controllers for the user and registers them with the tracker
// (call after tracker_start).
void move_start(int move_module, int user_id, MoveController out[MOVE_MAX]);
// Reads buttons and tracking; (un)registers controllers as they (dis)connect.
void move_update(MoveController ctl[MOVE_MAX]);
// RGB of a tracker LED colour index.
uint32_t move_led_rgb(uint32_t led_color);
