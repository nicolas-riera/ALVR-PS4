#pragma once

#include "move.h"
#include "vrmath.h"

// HTC Vive wand emulation from a PS Move, in the spirit of PSMoveServiceEx:
//   trackpad touch  MOVE held + roll (x) / pitch (y); starts at the pad centre on press
//   trackpad click  TRIANGLE (left) / SQUARE (right) + rotation
//   grip            CIRCLE (left) / CROSS (right)
//   menu            SQUARE (left) / TRIANGLE (right)
//   system          START
//   trigger         analog T (click when fully pressed)
// SELECT is never bound (the system uses it for screenshots).

enum Hand { HAND_LEFT = 0, HAND_RIGHT = 1 };

// Move index 0 (switched on first, magenta) is the right hand, index 1 (red) the left.
static inline Hand move_index_hand(int index) { return index == 0 ? HAND_RIGHT : HAND_LEFT; }

struct WandInput {
    bool pad_touch, pad_click;
    float pad_x, pad_y; // -1..1, x right, y up
    bool grip, menu, system;
    float trigger;      // 0..1
    bool trigger_click;
};

struct WandEmulator {
    bool dragging;
    Quat reference; // controller orientation when the drag started
    WandInput last;
};

// Rotation (radians) that moves the touch point from the centre to the pad edge.
#define WAND_PAD_RANGE_RAD 0.5f

void wand_update(WandEmulator *emu, Hand hand, const MoveController &move, WandInput *out);
