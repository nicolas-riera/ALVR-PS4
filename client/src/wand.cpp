#include "wand.h"

#include <math.h>
#include <string.h>

static Quat mul(Quat a, Quat b)
{
    return Quat{a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

static void clamp_unit(float *x, float *y)
{
    const float len = sqrtf(*x * *x + *y * *y);
    if (len > 1.0f) {
        *x /= len;
        *y /= len;
    }
}

void wand_alternate_point(Quat controller, Quat head, float *dir_x, float *dir_y, float *x, float *y)
{
    // Headset yaw: its forward made horizontal. Looking up or down, the forward minus its
    // vertical share of the up vector still points ahead (the top of the head then does).
    const Vec3 hf = rotate(head, v3(0, 0, -1)), hu = rotate(head, v3(0, 1, 0));
    float fx = hf.x - hu.x * hf.y, fz = hf.z - hu.z * hf.y;
    float flen = sqrtf(fx * fx + fz * fz);
    if (flen < 1e-4f) {
        fx = 0.0f;
        fz = -1.0f;
        flen = 1.0f;
    }
    fx /= flen;
    fz /= flen;
    // Controller forward (-Z, handle to ball) in the headset's yaw frame: y ahead, x right
    // (right = forward x up = (-fz, 0, fx)).
    const Vec3 c = rotate(controller, v3(0, 0, -1));
    const float clen = sqrtf(c.x * c.x + c.z * c.z);
    if (clen >= WAND_ALT_MIN_HORIZONTAL) {
        *dir_y = (c.x * fx + c.z * fz) / clen;
        *dir_x = (c.x * -fz + c.z * fx) / clen;
    } else if (*dir_x == 0.0f && *dir_y == 0.0f) {
        *dir_y = 1.0f; // never had a direction: ahead
    }
    // Pointing up: from WAND_ALT_UP_RAD below straight up to straight up, edge to centre.
    const float up = acosf(c.y > 1.0f ? 1.0f : c.y < -1.0f ? -1.0f : c.y); // angle from straight up
    const float m = up >= WAND_ALT_UP_RAD ? 1.0f : up / WAND_ALT_UP_RAD;
    *x = *dir_x * m;
    *y = *dir_y * m;
    clamp_unit(x, y);
}

void wand_update(WandEmulator *emu, Hand hand, const MoveController &move, const WandSettings &cfg, Quat head,
                 bool head_valid, WandInput *out)
{
    memset(out, 0, sizeof(*out));
    if (!move.connected) { // all released; a drag starts afresh after the reconnection
        emu->dragging = false;
        emu->last = *out;
        return;
    }
    const uint16_t b = move.buttons;
    const uint16_t other_btn = hand == HAND_LEFT ? MOVE_BUTTON_TRIANGLE : MOVE_BUTTON_SQUARE;
    const uint16_t grip_btn = hand == HAND_LEFT ? MOVE_BUTTON_CIRCLE : MOVE_BUTTON_CROSS;
    const uint16_t menu_btn = hand == HAND_LEFT ? MOVE_BUTTON_SQUARE : MOVE_BUTTON_TRIANGLE;
    const uint16_t touch_btn = cfg.swap ? other_btn : MOVE_BUTTON_MOVE;
    const uint16_t click_btn = cfg.swap ? MOVE_BUTTON_MOVE : other_btn;
    const bool touch_alt = cfg.swap ? cfg.alt_other : cfg.alt_move;
    const bool click_alt = cfg.swap ? cfg.alt_move : cfg.alt_other;

    const float *o = move.track.orientation;
    const Quat cur{o[0], o[1], o[2], o[3]};
    const bool clicking = (b & click_btn) != 0;
    const bool touching = clicking || (b & touch_btn);
    // The click button places the point while it is held, else the touch button.
    const bool alternate = touching && (clicking ? click_alt : touch_alt);
    if (touching && !alternate && !emu->dragging) {
        emu->dragging = true;
        emu->reference = cur; // the touch starts at the pad centre
    } else if (!touching || alternate) {
        emu->dragging = false;
    }

    if (alternate) {
        out->pad_touch = true;
        wand_alternate_point(cur, head_valid ? head : Quat{0, 0, 0, 1}, &emu->alt_dir_x, &emu->alt_dir_y, &out->pad_x,
                             &out->pad_y);
        out->pad_click = clicking;
    } else if (emu->dragging) {
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
        clamp_unit(&x, &y);
        out->pad_touch = true;
        out->pad_x = x;
        out->pad_y = y;
        out->pad_click = clicking;
    }
    out->grip = (b & grip_btn) != 0;
    out->menu = (b & menu_btn) != 0;
    out->system = (b & MOVE_BUTTON_START) != 0;
    out->trigger = move.trigger / 255.0f;
    // The T bit is set from ~16 % of the travel, so the click comes from the analog value:
    // on at 90 %, off below 80 % (a trigger resting near 90 % does not chatter).
    out->trigger_click = out->trigger >= (emu->last.trigger_click ? 0.8f : 0.9f);
    emu->last = *out;
}
