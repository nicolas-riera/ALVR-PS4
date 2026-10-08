#pragma once

#include "move.h"
#include "vrmath.h"

// HTC Vive wand emulation from a PS Move, in the spirit of PSMoveServiceEx:
//   trackpad touch  MOVE held (default; or TRIANGLE / SQUARE, see WandSettings)
//   trackpad click  TRIANGLE (left) / SQUARE (right) (default; or MOVE); a click also touches
//   grip            CIRCLE (left) / CROSS (right)
//   menu            SQUARE (left) / TRIANGLE (right)
//   system          START
//   trigger         analog T (click when fully pressed)
// Each trackpad button places the point its own way (WandSettings):
//   Default    the point starts at the pad centre on press and follows the controller's
//              roll (x) and pitch (y) since then
//   Alternate  the point is where the controller points, relative to the headset's yaw (as
//              Rec Room's locomotion): straight ahead = top, left = left, behind = bottom;
//              always at the edge, except pointing up: from 40 degrees below straight up it
//              moves in to the centre (reached pointing straight up). Pointing (almost)
//              straight down keeps the last direction.
// With both buttons held, the click button's way places the point.
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

struct WandSettings {
    bool swap;      // MOVE clicks and TRIANGLE / SQUARE touch (the other way round by default)
    bool alt_move;  // MOVE places the point the Alternate way
    bool alt_other; // TRIANGLE / SQUARE does
};

struct WandEmulator {
    bool dragging;  // a Default drag is going on
    Quat reference; // controller orientation when the drag started
    float alt_dir_x, alt_dir_y; // last Alternate direction (unit), kept while pointing down
    WandInput last;
};

// Rotation (radians) that moves the touch point from the centre to the pad edge.
#define WAND_PAD_RANGE_RAD 0.5f
// Alternate: the point moves in to the centre within this angle of straight up (radians).
#define WAND_ALT_UP_RAD (40.0f * 3.14159265f / 180.0f)
// Alternate: below this horizontal length of the controller's forward (pointing straight up
// or down, ~8 degrees), its direction keeps the last one.
#define WAND_ALT_MIN_HORIZONTAL 0.14f

// head: the headset orientation (tracker space), for the Alternate way; head_valid false
// when there is none (then the Alternate point is relative to the tracker's forward).
void wand_update(WandEmulator *emu, Hand hand, const MoveController &move, const WandSettings &cfg, Quat head,
                 bool head_valid, WandInput *out);

// Alternate point (pad x, y) for a controller and headset orientation; *dir_x, *dir_y hold
// the last direction (updated unless the controller points almost straight up or down).
void wand_alternate_point(Quat controller, Quat head, float *dir_x, float *dir_y, float *x, float *y);
