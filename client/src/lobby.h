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
};

// One eye (0 = left, 1 = right) into its own image.
void lobby_render_eye(uint32_t *pixels, int width, int height, int pitch, const LobbyView *view, int eye);
// Both eyes side by side (left half / right half), used by the host preview.
void lobby_render(uint32_t *pixels, int width, int height, int pitch, const LobbyView *view);
