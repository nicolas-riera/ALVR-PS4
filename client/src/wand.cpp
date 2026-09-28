#include "wand.h"

#include <math.h>
#include <string.h>

static Quat mul(Quat a, Quat b)
{
    return Quat{a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

void wand_update(WandEmulator *emu, Hand hand, const MoveController &move, WandInput *out)
{
    memset(out, 0, sizeof(*out));
    if (!move.connected)
        return;
    const uint16_t b = move.buttons;
    const uint16_t click_btn = hand == HAND_LEFT ? MOVE_BUTTON_TRIANGLE : MOVE_BUTTON_SQUARE;
    const uint16_t grip_btn = hand == HAND_LEFT ? MOVE_BUTTON_CIRCLE : MOVE_BUTTON_CROSS;
    const uint16_t menu_btn = hand == HAND_LEFT ? MOVE_BUTTON_SQUARE : MOVE_BUTTON_TRIANGLE;

    const float *o = move.track.orientation;
    const Quat cur{o[0], o[1], o[2], o[3]};
    const bool touching = (b & MOVE_BUTTON_MOVE) || (b & click_btn);
    if (touching && !emu->dragging) {
        emu->dragging = true;
        emu->reference = cur; // the touch starts at the pad centre
    } else if (!touching) {
        emu->dragging = false;
    }

    if (emu->dragging) {
        // Rotation since the press, in the controller's own frame (handle along +Z,
        // sphere forward along -Z). Horizontal follows the roll (clockwise seen from the
        // handle = right), vertical follows the pitch (inverted after the first test).
        Quat d = mul(conj(emu->reference), cur);
        if (d.w < 0) {
            d.x = -d.x;
            d.y = -d.y;
            d.z = -d.z;
            d.w = -d.w;
        }
        float x = -2.0f * d.z / WAND_PAD_RANGE_RAD;
        float y = -2.0f * d.x / WAND_PAD_RANGE_RAD;
        float len = sqrtf(x * x + y * y);
        if (len > 1.0f) {
            x /= len;
            y /= len;
        }
        out->pad_touch = true;
        out->pad_x = x;
        out->pad_y = y;
        out->pad_click = (b & click_btn) != 0;
    }
    out->grip = (b & grip_btn) != 0;
    out->menu = (b & menu_btn) != 0;
    out->system = (b & MOVE_BUTTON_START) != 0;
    out->trigger = move.trigger / 255.0f;
    out->trigger_click = (b & MOVE_BUTTON_T) != 0;
    emu->last = *out;
}
