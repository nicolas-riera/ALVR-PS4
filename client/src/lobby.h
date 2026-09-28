#pragma once

#include <stdint.h>

#include "vrmath.h"

// Lobby environment shown while no PC is streaming: a floor grid like the
// official ALVR client, plus a marker at the PS Camera. Rendered in software
// into a side-by-side stereo image (left eye in the left half).

struct EyeFov {
    float tan_left, tan_right, tan_up, tan_down;
};

struct LobbyView {
    Vec3 eye_pos[2]; // tracker space: origin at the PS Camera, metres
    Quat eye_rot[2];
    EyeFov fov[2];  // left, right
    float floor_y;  // tracker-space height of the floor
    struct Controller {
        bool visible;
        Vec3 pos;      // sphere centre, tracker space
        Quat rot;
        uint32_t rgb;  // sphere colour
        bool tracked;  // position currently seen by the camera
        bool pad_touch, pad_click;
        float pad_x, pad_y;
    } controllers[2];
    bool grey;              // headset lost the camera for too long: grey screen
    const char *info[6];    // info panel lines (nullptr-terminated)
    Vec3 info_pos;          // panel centre, tracker space
    float info_yaw;         // panel facing (radians around +Y; 0 faces +Z)
};

// One eye (0 = left, 1 = right) into its own image.
void lobby_render_eye(uint32_t *pixels, int width, int height, int pitch, const LobbyView *view, int eye);
// Both eyes side by side (left half / right half), used by the host preview.
void lobby_render(uint32_t *pixels, int width, int height, int pitch, const LobbyView *view);
