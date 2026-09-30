#include "settings.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "log.h"

#ifndef ALVR_PS4_DEV
#define ALVR_PS4_DEV 0
#endif

// Panel layout in metres (panel coordinates: x right, y up, origin at the centre).
static const float PANEL_W = 1.02f, PANEL_TOP = 0.31f, PANEL_BOTTOM = -0.53f;
static const float WIZARD_W = 0.90f, WIZARD_TOP = 0.19f, WIZARD_BOTTOM = -0.27f;
static const float CALIB_W = 1.00f, CALIB_TOP = 0.27f, CALIB_BOTTOM = -0.45f;
static const float LABEL_H = 0.022f, TITLE_H = 0.032f, TIP_H = 0.0135f;
static const float BTN_W = 0.07f, BTN_H = 0.05f;
static const float MINUS_X = 0.320f, PLUS_X = 0.405f; // left edges of the - / + buttons
// Trigger thresholds for a click, with hysteresis; held +/- buttons repeat.
static const float TRIGGER_PRESS = 0.55f, TRIGGER_RELEASE = 0.35f;
static const uint64_t REPEAT_DELAY_US = 450000, REPEAT_US = 50000;
static const uint64_t SAVE_DELAY_US = 2000000; // config written 2 s after the last change

enum Button { BTN_NONE = -1, BTN_HEIGHT_MINUS, BTN_HEIGHT_PLUS, BTN_HPRED_MINUS, BTN_HPRED_PLUS, BTN_PRED_MINUS,
              BTN_PRED_PLUS, BTN_RES_MINUS, BTN_RES_PLUS, BTN_RATE, BTN_CENTER, BTN_CLOSE, BTN_RESET, BTN_CONFIRM,
              BTN_CALIBRATE, BTN_CANCEL, BTN_VIBRATION, BTN_HUD, BTN_SURFACE, BTN_COUNT };

struct Rect {
    float x0, y0, x1, y1;
};

// Settings panel: the title stays at the top and Close at the bottom; the rows between
// scroll inside [VIEW_BOTTOM, VIEW_TOP] (grab the panel or the scroll bar with the laser and
// drag, or the DualShock 4's right stick). Row centres below are at scroll 0.
static const float ROW_TITLE = 0.255f, ROW_ACTIONS = -0.470f;
static const float VIEW_TOP = 0.215f, VIEW_BOTTOM = -0.415f;
static const float ROW_USER = 0.165f, ROW_CAMERA = 0.095f, ROW_TIP = 0.045f, ROW_HPRED = -0.035f,
                   ROW_PRED = -0.115f, ROW_RES = -0.195f, ROW_RATE = -0.275f, ROW_CENTER = -0.355f,
                   ROW_VIBRATION = -0.435f, ROW_HUD = -0.515f, ROW_RESET = -0.605f;
static const float CONTENT_BOTTOM = ROW_RESET - 0.05f;
static const float SCROLL_MAX = VIEW_BOTTOM - CONTENT_BOTTOM;
static const float SCROLL_STICK_M_PER_S = 0.6f; // DualShock 4 right stick fully pushed
// Scroll bar on the panel's right edge; it can be grabbed anywhere right of the buttons.
static const float BAR_X0 = PANEL_W / 2 - 0.018f, BAR_X1 = PANEL_W / 2 - 0.006f, BAR_GRAB_X0 = PLUS_X + BTN_W + 0.003f;
static const float BAR_VIEW_H = VIEW_TOP - VIEW_BOTTOM, BAR_THUMB_H = BAR_VIEW_H * BAR_VIEW_H / (BAR_VIEW_H + SCROLL_MAX);
// Vibration strength slider (panel x), in 10 % steps.
static const float SLIDER_X0 = 0.000f, SLIDER_X1 = PLUS_X + BTN_W;
// Row centres of the first launch wizard.
static const float WROW_TITLE = 0.135f, WROW_TEXT = 0.075f, WROW_USER = 0.000f, WROW_CALIB = -0.070f,
                   WROW_TIP = -0.135f, WROW_CONFIRM = -0.205f;
// Height calibration panel: title, the two pictogram boxes (T-pose left, floor right), the
// last attempt's hint and Cancel.
static const float CROW_TITLE = 0.215f, CROW_TEXT = 0.165f, CBOX_TOP = 0.130f, CBOX_BOTTOM = -0.300f,
                   CROW_GROUND = -0.090f, CROW_CAPTION1 = -0.140f, CROW_CAPTION2 = -0.180f, CROW_STATUS = -0.255f,
                   CROW_HINT = -0.335f, CROW_CANCEL = -0.395f;
static const float CBOX_X0 = 0.02f, CBOX_X1 = 0.48f; // each box spans +-x0..+-x1
static const uint64_t RESET_CONFIRM_US = 3000000; // second click within 3 s confirms
static const uint64_t WIZARD_NO_MOVE_US = 20000000; // no PS Move to click with: go on after 20 s
static const uint64_t CALIB_HINT_US = 5000000;      // a failed attempt's hint stays 5 s
static const uint64_t CALIB_RESULT_US = 8000000;    // the result stays 8 s in the panel's tip line
static const int DEFAULT_USER_HEIGHT_CM = SETTINGS_DEFAULT_USER_HEIGHT_CM;
static const float EYE_HEIGHT_RATIO = SETTINGS_EYE_HEIGHT_RATIO;
// Height calibration. Floor: the controller touching the floor is at least this far below
// the highest the head was since the calibration opened (standing; a hand hanging down is
// ~0.8 m below the eyes).
static const float FLOOR_BELOW_HEAD_M = 0.90f;
// Height of the tracked point above the floor, PS Move only (the DualShock 4's tracking is
// too coarse for it). The sphere's radius (Sony gives 46 mm for the controller's largest
// diameter, PSMoveService models the sphere with a 2.25 cm radius): 2.3 cm, plus the
// tracked point's offset from the sphere centre towards the handle, found with the Dev
// surface test (same table, hardware test 2026-09-30): the point is 8 mm higher with the
// ball down than with the Move lying on its side.
static const float MOVE_BALL_RADIUS_M = 0.023f, MOVE_POINT_TOWARDS_HANDLE_M = 0.008f;
// T-pose: both PS Moves pointing straight up (within TPOSE_UP_MIN_Y of their forward): the
// spheres are then right above the fists. Pointed sideways they were ~10 cm further out
// each (hardware tests: 1.63-1.72 m between the spheres against 1.48-1.54 m pointed up),
// and the angle changed the result. Measured between the hands (8 cm behind each sphere
// along the controller), which pointed up is the distance between the spheres.
// Hands apart / body height, for a 1.73 m user (hardware tests, Moves pointed up): 0.876
// gave 1.71-1.72 m in the last test (hands 1.491-1.500 m), so 0.868.
static const float MOVE_HAND_BEHIND_SPHERE_M = 0.08f;
static const float TPOSE_HANDS_PER_HEIGHT = 0.868f;
static const float TPOSE_UP_MIN_Y = 0.94f; // forward.y: within ~20 degrees of straight up
static const float TPOSE_MIN_SPAN_M = 0.90f, TPOSE_MAX_LEVEL_M = 0.25f, TPOSE_MAX_OFF_CENTRE_M = 0.35f;
static const float TPOSE_ABOVE_HEAD_M = 0.15f, TPOSE_BELOW_HEAD_M = 0.55f; // hands' height band around the eyes

struct Pointer {
    bool valid, hit;
    Vec3 from, to;
    float px, py;    // where the ray meets the panel's plane (panel coordinates), when it does
    bool on_plane;
    int hover; // Button under the laser
    bool pressed;
    int held;  // button being held (+/- repeat, slider)
    uint64_t next_repeat_us;
    bool dragging;   // grabbed the panel or the scroll bar: scrolling
    bool bar;        // grabbed the scroll bar (the rows move the other way)
    float drag_py, drag_scroll;
};

static struct {
    bool open;
    bool wizard;             // first launch: height and Confirm only, cannot be closed
    bool calib;              // height calibration shown (it goes back to the wizard or the settings)
    bool surface_test;       // Dev: calibration presses only log the surface under the controller
    uint64_t no_move_since;  // wizard without any PS Move connected, since then (0: one is on)
    Vec3 origin;
    Pointer ptr[LOBBY_POINTERS];
    bool dirty;
    uint64_t last_change_us, last_update_us;
    uint64_t reset_armed_us; // first Reset click, waiting for the confirmation
    bool measured;           // the floor comes from a calibration: Confirm keeps it
    float calib_head_max;    // highest head (tracker y) since the calibration opened
    uint64_t hint_until, result_until;
    float scroll;            // settings rows moved up by this much (0..SCROLL_MAX)
    bool height_touched;     // wizard: height set with - / + or measured (the floor is no longer a guess)
    float head_smooth;       // head height, smoothed over ~0.3 s (the height shown follows it)
    bool head_smooth_valid;
    int shown_height_cm;     // height shown (0: none yet)
    // Values shown, refreshed by settings_update.
    char user[48], camera[48], tip[96], hpred[48], pred[32], res[48], rate[16], vibration[16];
    char hint[128], result[96];     // calibration: last failed attempt; result shown in the tip
    char tpose_status[64], floor_status[64];
    uint32_t tpose_rgb, floor_rgb;  // status colours
    bool tpose_ready, floor_ready;  // the pose can be done now (box outlined in green)
} g;

static const uint32_t STATUS_READY_RGB = 0x60d080, STATUS_WAIT_RGB = 0x8090a0, STATUS_WARN_RGB = 0xffa030;

static bool is_settings() { return !g.calib && !g.wizard; }

// Content row of a settings button (0 for the fixed ones).
static float button_row(int b)
{
    switch (b) {
    case BTN_HEIGHT_MINUS:
    case BTN_HEIGHT_PLUS: return ROW_USER;
    case BTN_CALIBRATE: return ROW_CAMERA;
    case BTN_HPRED_MINUS:
    case BTN_HPRED_PLUS: return ROW_HPRED;
    case BTN_PRED_MINUS:
    case BTN_PRED_PLUS: return ROW_PRED;
    case BTN_RES_MINUS:
    case BTN_RES_PLUS: return ROW_RES;
    case BTN_RATE: return ROW_RATE;
    case BTN_CENTER: return ROW_CENTER;
    case BTN_VIBRATION: return ROW_VIBRATION;
    case BTN_HUD: return ROW_HUD;
    case BTN_RESET: return ROW_RESET;
    }
    return 0.0f;
}

// A scrolled row (its centre at scroll 0, half its height) is shown only when it fits in
// the viewport; its panel y is row + scroll.
static bool row_visible(float row, float half)
{
    const float y = row + g.scroll;
    return y + half <= VIEW_TOP && y - half >= VIEW_BOTTOM;
}

// Top of the scroll bar's thumb, and the scroll that puts it at a given top.
static float thumb_top(float scroll) { return VIEW_TOP - scroll / SCROLL_MAX * (BAR_VIEW_H - BAR_THUMB_H); }

static float clamp_scroll(float s) { return s < 0.0f ? 0.0f : s > SCROLL_MAX ? SCROLL_MAX : s; }

static float scroll_for_thumb(float top) { return clamp_scroll((VIEW_TOP - top) / (BAR_VIEW_H - BAR_THUMB_H) * SCROLL_MAX); }

// The laser is on the scroll bar (its column right of the buttons, over the viewport).
static bool on_bar(float px, float py) { return is_settings() && px >= BAR_GRAB_X0 && py <= VIEW_TOP && py >= VIEW_BOTTOM; }

static bool button_active(int b)
{
    if (g.calib)
        return b == BTN_CANCEL || (ALVR_PS4_DEV && b == BTN_SURFACE);
    if (g.wizard)
        return b == BTN_HEIGHT_MINUS || b == BTN_HEIGHT_PLUS || b == BTN_CALIBRATE || b == BTN_CONFIRM;
    if (b == BTN_CONFIRM || b == BTN_CANCEL || b == BTN_SURFACE)
        return false;
    return b == BTN_CLOSE || row_visible(button_row(b), b == BTN_RESET ? 0.03f : BTN_H / 2);
}

static void panel_bounds(float *half_w, float *top, float *bottom)
{
    *half_w = (g.calib ? CALIB_W : g.wizard ? WIZARD_W : PANEL_W) / 2;
    *top = g.calib ? CALIB_TOP : g.wizard ? WIZARD_TOP : PANEL_TOP;
    *bottom = g.calib ? CALIB_BOTTOM : g.wizard ? WIZARD_BOTTOM : PANEL_BOTTOM;
}

static Rect button_rect(int b)
{
    auto pm = [](float x, float row) { return Rect{x, row - BTN_H / 2, x + BTN_W, row + BTN_H / 2}; };
    auto wide = [](float row) { return Rect{MINUS_X, row - BTN_H / 2, PLUS_X + BTN_W, row + BTN_H / 2}; };
    if (g.calib) {
        if (b == BTN_CANCEL)
            return Rect{-0.15f, CROW_CANCEL - 0.03f, 0.15f, CROW_CANCEL + 0.03f};
        if (b == BTN_SURFACE)
            return Rect{-CALIB_W / 2 + 0.02f, CROW_CANCEL - 0.025f, -CALIB_W / 2 + 0.25f, CROW_CANCEL + 0.025f};
        return Rect{0, 0, 0, 0};
    }
    if (g.wizard) {
        // The - / + pair sits at the same x as in the settings, shifted into the narrower panel.
        const float dx = -(PANEL_W - WIZARD_W) / 2;
        switch (b) {
        case BTN_HEIGHT_MINUS: return pm(MINUS_X + dx, WROW_USER);
        case BTN_HEIGHT_PLUS: return pm(PLUS_X + dx, WROW_USER);
        case BTN_CALIBRATE: return Rect{MINUS_X + dx, WROW_CALIB - BTN_H / 2, PLUS_X + dx + BTN_W, WROW_CALIB + BTN_H / 2};
        case BTN_CONFIRM: return Rect{-0.15f, WROW_CONFIRM - 0.03f, 0.15f, WROW_CONFIRM + 0.03f};
        }
        return Rect{0, 0, 0, 0};
    }
    if (b == BTN_CLOSE)
        return Rect{-0.15f, ROW_ACTIONS - 0.03f, 0.15f, ROW_ACTIONS + 0.03f};
    const float row = button_row(b) + g.scroll;
    switch (b) {
    case BTN_HEIGHT_MINUS:
    case BTN_HPRED_MINUS:
    case BTN_PRED_MINUS:
    case BTN_RES_MINUS: return pm(MINUS_X, row);
    case BTN_HEIGHT_PLUS:
    case BTN_HPRED_PLUS:
    case BTN_PRED_PLUS:
    case BTN_RES_PLUS: return pm(PLUS_X, row);
    case BTN_CALIBRATE:
    case BTN_RATE:
    case BTN_CENTER:
    case BTN_HUD: return wide(row);
    case BTN_VIBRATION: return Rect{SLIDER_X0 - 0.015f, row - BTN_H / 2, BAR_GRAB_X0 - 0.001f, row + BTN_H / 2};
    case BTN_RESET: return Rect{-0.19f, row - 0.03f, 0.19f, row + 0.03f};
    }
    return Rect{0, 0, 0, 0};
}

static void format_height(char *out, size_t n, float m)
{
    int inches = (int)lroundf(m / 0.0254f);
    if (inches < 0)
        inches = 0;
    snprintf(out, n, "%.2f m  /  %d ft %d in", m, inches / 12, inches % 12);
}

static void save_now(const ClientConfig *cfg)
{
    config_store(cfg);
    g.dirty = false;
    LOG("settings: saved (user height %d cm, camera height %d cm, headset prediction %d%%, controller prediction %d ms, "
        "resolution %d%%, %d Hz, center on SteamVR start %d, vibration %d%%, overlay %d)",
        cfg->user_height_cm, cfg->camera_height_cm, cfg->head_prediction_percent, cfg->controller_prediction_ms,
        cfg->resolution_percent, cfg->refresh_rate, cfg->center_on_connect, cfg->vibration_percent, cfg->hud);
}

static ClientConfig *g_cfg; // last config seen, to save on close
// Values in use since the launch, for the settings that apply at the next one: their
// "(restart required)" turns orange once changed.
static int g_launch_resolution = -1, g_launch_refresh_rate = -1;
static const uint32_t RESTART_RGB = 0x8090a0, RESTART_CHANGED_RGB = 0xffa030;

static void open_panel(Vec3 head_pos, bool wizard)
{
    g.open = true;
    g.wizard = wizard;
    g.calib = false;
    g.surface_test = false;
    g.measured = false;
    g.result_until = 0;
    g.no_move_since = 0;
    g.scroll = 0.0f;
    g.head_smooth_valid = false;
    g.shown_height_cm = 0;
    if (wizard)
        g.height_touched = false;
    // In front of the head towards the camera (-Z), slightly below eye level; the head
    // orientation is ignored so the panel always faces the play area's forward.
    g.origin = head_pos + v3(0.0f, -0.12f, -0.85f);
    for (Pointer &p : g.ptr) {
        p.pressed = true; // wait for a trigger release first
        p.held = BTN_NONE;
        p.dragging = false;
    }
    g.reset_armed_us = 0;
    LOG("settings: opened%s", wizard ? " (first launch wizard)" : "");
}

static void close_panel()
{
    if (!g.open)
        return;
    g.open = false;
    g.wizard = false;
    g.calib = false;
    if (g.dirty && g_cfg)
        save_now(g_cfg);
    LOG("settings: closed");
}

void settings_open(Vec3 head_pos)
{
    open_panel(head_pos, false);
}

void settings_open_wizard(Vec3 head_pos)
{
    open_panel(head_pos, true);
}

void settings_close()
{
    if (!g.wizard) // the wizard only closes with Confirm
        close_panel();
}

bool settings_is_open()
{
    return g.open;
}

bool settings_is_wizard()
{
    return g.open && g.wizard;
}

bool settings_wizard_height_set()
{
    return g.height_touched;
}

// The user stands straight: the floor goes below the headset at the eye height of their
// height. The camera is the tracker origin, so the floor is kept as the camera height,
// which stays valid after tracking resets.
static void place_floor(ClientConfig *c, const SettingsContext &ctx)
{
    const float head_y = g.head_smooth_valid ? g.head_smooth : ctx.head_y;
    const int cm = (int)lroundf((c->user_height_cm * EYE_HEIGHT_RATIO / 100.0f - head_y) * 100.0f);
    c->camera_height_cm = cm < 1 ? 1 : cm > 300 ? 300 : cm;
}

static unsigned apply(int b, const SettingsContext &ctx, uint64_t now)
{
    ClientConfig *c = ctx.config;
    if (b != BTN_RESET)
        g.reset_armed_us = 0;
    switch (b) {
    case BTN_RESET:
        if (!g.reset_armed_us || now - g.reset_armed_us > RESET_CONFIRM_US) {
            g.reset_armed_us = now;
            return 0;
        }
        // Every setting back to its default, the height included (the hostname stays: the
        // PC trusts the headset by it). The panel closes and the height wizard comes back.
        g.reset_armed_us = 0;
        c->resolution_percent = 130;
        c->controller_prediction_ms = 0;
        c->head_prediction_percent = CONFIG_DEFAULT_HEAD_PREDICTION;
        c->camera_height_cm = 0;
        c->user_height_cm = 0;
        c->center_on_connect = 1;
        c->refresh_rate = 90;
        c->vibration_percent = 100;
        c->hud = 0;
        LOG("settings: reset to defaults");
        return SETTINGS_RESET;
    case BTN_HEIGHT_MINUS:
    case BTN_HEIGHT_PLUS: {
        // From the height shown (the headset's now): the floor moves under the headset.
        int h = g.shown_height_cm > 0 ? g.shown_height_cm : c->user_height_cm > 0 ? c->user_height_cm : DEFAULT_USER_HEIGHT_CM;
        h += b == BTN_HEIGHT_PLUS ? 1 : -1;
        g.height_touched = true;
        c->user_height_cm = h < 100 ? 100 : h > 230 ? 230 : h;
        place_floor(c, ctx);
        g.measured = false;
        g.result_until = 0;
        return SETTINGS_HEIGHT_CHANGED;
    }
    case BTN_CONFIRM:
        // The floor comes from the headset height at this moment (standing straight),
        // unless it was just measured by the calibration.
        if (c->user_height_cm <= 0)
            c->user_height_cm = DEFAULT_USER_HEIGHT_CM;
        if (!g.measured)
            place_floor(c, ctx);
        LOG("settings: first launch height confirmed: %d cm (camera height %d cm%s)", c->user_height_cm,
            c->camera_height_cm, g.measured ? ", calibrated" : "");
        return SETTINGS_HEIGHT_CHANGED | SETTINGS_CONFIRMED;
    case BTN_CALIBRATE:
        g.calib = true;
        g.surface_test = false;
        g.calib_head_max = ctx.head_y;
        g.hint_until = 0;
        LOG("settings: height calibration opened (head y=%.3f)", ctx.head_y);
        return 0;
    case BTN_CANCEL:
        g.calib = false;
        LOG("settings: height calibration %s", g.surface_test ? "closed (surface test)" : "canceled");
        g.surface_test = false;
        return 0;
    case BTN_SURFACE:
        g.surface_test = !g.surface_test;
        g.hint_until = 0;
        LOG("settings: surface test %s", g.surface_test ? "on" : "off");
        return 0;
    case BTN_CENTER:
        c->center_on_connect = !c->center_on_connect;
        return 0;
    case BTN_HUD:
        c->hud = !c->hud;
        return 0;
    case BTN_RATE:
        c->refresh_rate = c->refresh_rate == 90 ? 60 : 90;
        return 0;
    case BTN_HPRED_MINUS:
    case BTN_HPRED_PLUS: {
        int v = c->head_prediction_percent + (b == BTN_HPRED_PLUS ? 10 : -10);
        c->head_prediction_percent = v < 0 ? 0 : v > 100 ? 100 : v;
        return 0;
    }
    case BTN_PRED_MINUS:
    case BTN_PRED_PLUS: {
        int v = c->controller_prediction_ms + (b == BTN_PRED_PLUS ? 5 : -5);
        c->controller_prediction_ms = v < 0 ? 0 : v > 60 ? 60 : v;
        return 0;
    }
    case BTN_RES_MINUS:
    case BTN_RES_PLUS: {
        int v = c->resolution_percent + (b == BTN_RES_PLUS ? 10 : -10);
        c->resolution_percent = v < 50 ? 50 : v > 160 ? 160 : v;
        return 0;
    }
    }
    return 0;
}

static bool is_repeatable(int b)
{
    return b >= BTN_HEIGHT_MINUS && b <= BTN_RES_PLUS;
}

// Vibration slider: the value under the laser, in 10 % steps (far left: off).
static int slider_value(float px)
{
    float f = (px - SLIDER_X0) / (SLIDER_X1 - SLIDER_X0);
    f = f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f;
    return (int)lroundf(f * 10.0f) * 10;
}

// ---- Height calibration ----------------------------------------------------------------

static void set_hint(const char *s, uint64_t now)
{
    snprintf(g.hint, sizeof(g.hint), "%s", s);
    g.hint_until = now + CALIB_HINT_US;
}

static int clamp_height_cm(float cm)
{
    const int h = (int)lroundf(cm);
    return h < 100 ? 100 : h > 230 ? 230 : h;
}

static const char *ray_name(int i)
{
    return i == 0 ? "PS Move 0" : "PS Move 1";
}

// Height of a PS Move's tracked point above the surface its ball rests on (forward: from
// the handle to the ball).
static float move_floor_offset(Vec3 forward)
{
    const float down = forward.y < -1.0f ? 1.0f : forward.y > 0.0f ? 0.0f : -forward.y;
    return MOVE_BALL_RADIUS_M + MOVE_POINT_TOWARDS_HANDLE_M * down;
}

// Success: the floor at floor_y (tracker space) and the height found; back to the panel
// the calibration came from.
static unsigned calibrated(ClientConfig *c, float floor_y, int height_cm, uint64_t now)
{
    const int cam = (int)lroundf(-floor_y * 100.0f);
    c->camera_height_cm = cam < 1 ? 1 : cam > 300 ? 300 : cam;
    c->user_height_cm = height_cm;
    g.measured = true;
    g.height_touched = true;
    g.calib = false;
    g.result_until = now + CALIB_RESULT_US;
    return SETTINGS_HEIGHT_CHANGED | SETTINGS_CALIBRATED;
}

// A PS Move touching the floor, trigger pulled: the floor is right under its tracked point.
// The height shown comes from the head standing (its highest since the calibration opened).
static unsigned calibrate_floor(int i, const SettingsRay &r, const SettingsContext &ctx, uint64_t now)
{
    ClientConfig *c = ctx.config;
    const float floor_y = r.origin.y - move_floor_offset(r.dir);
    const float eye_m = g.calib_head_max - floor_y;
    const int h = clamp_height_cm(eye_m / EYE_HEIGHT_RATIO * 100.0f);
    // With the height entered before: the eye height ratio it would take (checks
    // SETTINGS_EYE_HEIGHT_RATIO against the tracker's eye position).
    char ratio[48] = "";
    if (c->user_height_cm > 0)
        snprintf(ratio, sizeof(ratio), ", eye height / that height = %.3f", eye_m / (c->user_height_cm / 100.0f));
    LOG("settings: floor touched with %s at y=%.3f: floor y=%.3f (was %.3f), standing eyes %.3f m above it -> "
        "height %d cm (was %d cm%s)",
        ray_name(i), r.origin.y, floor_y, ctx.floor_y, eye_m, h, c->user_height_cm, ratio);
    snprintf(g.result, sizeof(g.result), "Floor measured. Your height: about %.2f m", h / 100.0f);
    return calibrated(c, floor_y, h, now);
}

// Both PS Moves held out to the sides at shoulder height, both triggers pulled: the height
// comes from the arm span, the floor from the head at that moment (standing).
static unsigned calibrate_tpose(const SettingsRay rays[LOBBY_POINTERS], const SettingsContext &ctx, uint64_t now)
{
    // The hands, behind the spheres along each controller (see MOVE_HAND_BEHIND_SPHERE_M).
    const Vec3 a = rays[0].origin - rays[0].dir * MOVE_HAND_BEHIND_SPHERE_M;
    const Vec3 b = rays[1].origin - rays[1].dir * MOVE_HAND_BEHIND_SPHERE_M;
    const Vec3 d = a - b, mid = (a + b) * 0.5f, ds = rays[0].origin - rays[1].origin;
    const float span = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
    const float spheres = sqrtf(ds.x * ds.x + ds.y * ds.y + ds.z * ds.z);
    const float off_centre = sqrtf((mid.x - ctx.head.x) * (mid.x - ctx.head.x) + (mid.z - ctx.head.z) * (mid.z - ctx.head.z));
    if (!rays[0].seen || !rays[1].seen) {
        set_hint("Both PS Moves must be seen by the camera: face it, arms out to the sides", now);
        return 0;
    }
    if (rays[0].dir.y < TPOSE_UP_MIN_Y || rays[1].dir.y < TPOSE_UP_MIN_Y) {
        set_hint("Point both PS Moves straight up (balls on top), arms out to the sides", now);
        LOG("settings: T-pose refused: forward y %+.2f %+.2f (pointing up needs %.2f)", rays[0].dir.y, rays[1].dir.y,
            TPOSE_UP_MIN_Y);
        return 0;
    }
    if (span < TPOSE_MIN_SPAN_M || off_centre > TPOSE_MAX_OFF_CENTRE_M) {
        set_hint("Spread your arms fully, straight out to the sides", now);
        LOG("settings: T-pose refused: hands %.2f m apart, their middle %.2f m from the head", span, off_centre);
        return 0;
    }
    if (fabsf(d.y) > TPOSE_MAX_LEVEL_M || mid.y > ctx.head_y + TPOSE_ABOVE_HEAD_M ||
        mid.y < ctx.head_y - TPOSE_BELOW_HEAD_M) {
        set_hint("Hold both arms level, at shoulder height", now);
        LOG("settings: T-pose refused: hands at y=%.2f and %.2f, head at %.2f", a.y, b.y, ctx.head_y);
        return 0;
    }
    ClientConfig *c = ctx.config;
    const int h = clamp_height_cm(span / TPOSE_HANDS_PER_HEIGHT * 100.0f);
    const float floor_y = ctx.head_y - h * EYE_HEIGHT_RATIO / 100.0f;
    LOG("settings: T-pose: hands %.3f m apart (spheres %.3f, forward y %+.2f %+.2f) -> height %d cm (was %d cm), "
        "floor y=%.3f (was %.3f)",
        span, spheres, rays[0].dir.y, rays[1].dir.y, h, c->user_height_cm, floor_y, ctx.floor_y);
    snprintf(g.result, sizeof(g.result), "Arm span measured. Your height: about %.2f m", h / 100.0f);
    return calibrated(c, floor_y, h, now);
}

// Dev surface test: a PS Move resting on a surface the camera sees (a table), trigger
// pulled: logs its tracked height and the surface found with the offsets above, applied to
// nothing. The ball down and the Move lying on its side must give the same surface.
static void surface_test(int i, const SettingsRay &r, const SettingsContext &ctx, uint64_t now)
{
    const float offset = move_floor_offset(r.dir);
    const float surface_y = r.origin.y - offset;
    // Pose: the Move's forward (-Z, from the handle to the ball) pointing down = ball down;
    // level = lying on its side.
    const char *pose = r.dir.y < -0.8f ? "ball down" : fabsf(r.dir.y) < 0.35f ? "lying" : "tilted";
    LOG("settings: surface test: %s %s (forward y=%+.2f), tracked y=%.4f (%s), offset %.1f cm -> surface y=%.4f, "
        "%.3f m below the camera (head y=%.3f)",
        ray_name(i), pose, r.dir.y, r.origin.y, r.seen ? "seen by the camera" : "NOT seen by the camera",
        offset * 100.0f, surface_y, -surface_y, ctx.head_y);
    char s[128];
    snprintf(s, sizeof(s), "%s %s: surface %.3f m below the camera%s", i ? "Move 1" : "Move 0", pose, -surface_y,
             r.seen ? "" : " (not seen!)");
    set_hint(s, now);
    g.hint_until = now + 60000000; // until the next press
}

// Once per frame while the calibration is shown: what can be done now (shown under each
// pictogram), and the attempts made by the trigger presses of this frame.
static unsigned calibration_update(const SettingsRay rays[LOBBY_POINTERS], const bool pressed_now[LOBBY_POINTERS],
                                   const SettingsContext &ctx, uint64_t now)
{
    if (ctx.head_y > g.calib_head_max)
        g.calib_head_max = ctx.head_y;
    const float low_y = g.calib_head_max - FLOOR_BELOW_HEAD_M;
    const bool moves = rays[0].valid && rays[1].valid, moves_seen = moves && rays[0].seen && rays[1].seen;
    int low = -1, low_seen = -1;
    for (int i = 0; i < SETTINGS_RAY_PAD; i++) // PS Moves only
        if (rays[i].valid && rays[i].origin.y <= low_y) {
            low = i;
            if (rays[i].seen)
                low_seen = i;
        }
    const bool moves_up = moves && rays[0].dir.y >= TPOSE_UP_MIN_Y && rays[1].dir.y >= TPOSE_UP_MIN_Y;
    g.tpose_ready = moves_seen && moves_up;
    snprintf(g.tpose_status, sizeof(g.tpose_status), "%s",
             !moves        ? "Needs two PS Moves"
             : !moves_seen ? "Both PS Moves must be seen by the camera"
             : !moves_up   ? "Point both PS Moves straight up"
                           : "Ready: pull both triggers");
    g.tpose_rgb = g.tpose_ready ? STATUS_READY_RGB : moves ? STATUS_WARN_RGB : STATUS_WAIT_RGB;
    g.floor_ready = low_seen >= 0;
    snprintf(g.floor_status, sizeof(g.floor_status), "%s",
             low_seen >= 0 ? "Controller seen near the floor" : low >= 0 ? "The camera cannot see it there"
                                                                         : "Waiting for a controller near the floor");
    g.floor_rgb = low_seen >= 0 ? STATUS_READY_RGB : low >= 0 ? STATUS_WARN_RGB : STATUS_WAIT_RGB;

    for (int i = 0; i < LOBBY_POINTERS; i++) {
        if (!pressed_now[i] || !rays[i].valid)
            continue;
        if (i == SETTINGS_RAY_PAD) {
            set_hint("Use a PS Move: the DualShock 4 is not tracked precisely enough", now);
            continue;
        }
        if (g.surface_test) {
            surface_test(i, rays[i], ctx, now);
            continue;
        }
        // Both PS Move triggers held (the second press makes the attempt): T-pose, unless
        // the pressing Move is down at the floor (the other hand may just grip tightly).
        if (i < SETTINGS_RAY_PAD && moves && rays[0].trigger >= TRIGGER_PRESS && rays[1].trigger >= TRIGGER_PRESS &&
            rays[i].origin.y > low_y)
            return calibrate_tpose(rays, ctx, now);
        if (rays[i].origin.y <= low_y) {
            if (rays[i].seen)
                return calibrate_floor(i, rays[i], ctx, now);
            set_hint("The camera cannot see the controller there: move further from the camera", now);
            continue;
        }
        set_hint("Touch the floor with the ball, or spread your arms and pull both triggers", now);
    }
    return 0;
}

unsigned settings_update(const SettingsContext &ctx, const SettingsRay rays[LOBBY_POINTERS], uint64_t now,
                         int *clicked_hand)
{
    *clicked_hand = -1;
    g_cfg = ctx.config;
    if (g_launch_resolution < 0) {
        g_launch_resolution = g_cfg->resolution_percent;
        g_launch_refresh_rate = g_cfg->refresh_rate;
    }
    float dt = g.last_update_us && now > g.last_update_us ? (now - g.last_update_us) / 1e6f : 0.0f;
    if (dt > 0.1f) // first update since the panel opened, or a long frame
        dt = 0.1f;
    g.last_update_us = now;
    if (!g.open)
        return 0;
    unsigned actions = 0;
    const ClientConfig before = *ctx.config;
    ClientConfig *cfg = ctx.config;
    bool close = false;
    bool pressed_now[LOBBY_POINTERS] = {}; // trigger pressed this frame, not on a button (calibration)
    const bool calib = g.calib;
    float half_w, top, bottom;
    panel_bounds(&half_w, &top, &bottom);
    for (int i = 0; i < LOBBY_POINTERS; i++) {
        Pointer &p = g.ptr[i];
        const SettingsRay &r = rays[i];
        p.valid = r.valid;
        p.hit = false;
        p.on_plane = false;
        p.hover = BTN_NONE;
        if (!r.valid) {
            p.dragging = false;
            continue;
        }
        p.from = r.origin;
        p.to = r.origin + r.dir * 1.5f;
        // The panel faces +Z: intersect the plane z = origin.z.
        if (r.dir.z < -1e-3f) {
            float t = (g.origin.z - r.origin.z) / r.dir.z;
            if (t > 0.0f) {
                Vec3 hit = r.origin + r.dir * t;
                p.px = hit.x - g.origin.x;
                p.py = hit.y - g.origin.y;
                p.on_plane = true;
                if (p.px >= -half_w && p.px <= half_w && p.py >= bottom && p.py <= top) {
                    p.hit = true;
                    p.to = hit;
                    for (int b = 0; b < BTN_COUNT; b++) {
                        if (!button_active(b))
                            continue;
                        Rect rc = button_rect(b);
                        if (p.px >= rc.x0 && p.px <= rc.x1 && p.py >= rc.y0 && p.py <= rc.y1)
                            p.hover = b;
                    }
                }
            }
        }
        // Trigger: a click on press, then repeats while held on a +/- button; the slider
        // follows the laser while held; pressed elsewhere on the rows, the panel is grabbed
        // and scrolls with the laser.
        if (!p.pressed && r.trigger >= TRIGGER_PRESS) {
            p.pressed = true;
            p.held = p.hover;
            pressed_now[i] = p.hover == BTN_NONE;
            if (p.hover == BTN_NONE && p.hit && is_settings() && p.py <= VIEW_TOP && p.py >= VIEW_BOTTOM) {
                p.dragging = true;
                p.bar = on_bar(p.px, p.py);
                // On the bar's track outside the thumb: the thumb jumps there (centred on the laser).
                const float top = thumb_top(g.scroll);
                if (p.bar && (p.py > top || p.py < top - BAR_THUMB_H))
                    g.scroll = scroll_for_thumb(p.py + BAR_THUMB_H / 2);
                p.drag_py = p.py;
                p.drag_scroll = g.scroll;
            } else if (p.hover == BTN_VIBRATION) {
                cfg->vibration_percent = slider_value(p.px);
            } else if (p.hover != BTN_NONE) {
                *clicked_hand = i;
                if (p.hover == BTN_CLOSE)
                    close = true;
                else
                    actions |= apply(p.hover, ctx, now);
                p.next_repeat_us = now + REPEAT_DELAY_US;
            }
        } else if (p.pressed && r.trigger <= TRIGGER_RELEASE) {
            if (p.held == BTN_VIBRATION) {
                *clicked_hand = i;
                actions |= SETTINGS_VIBRATION_SET;
                LOG("settings: vibration %d%%", cfg->vibration_percent);
            }
            p.pressed = false;
            p.held = BTN_NONE;
            p.dragging = false;
            p.bar = false;
        } else if (p.pressed && p.held == BTN_VIBRATION && p.on_plane) {
            cfg->vibration_percent = slider_value(p.px);
        } else if (p.pressed && p.dragging && p.on_plane) {
            if (p.bar) // the thumb follows the laser
                g.scroll = scroll_for_thumb(thumb_top(p.drag_scroll) + (p.py - p.drag_py));
            else // the rows follow the laser
                g.scroll = clamp_scroll(p.drag_scroll + (p.py - p.drag_py));
        } else if (p.pressed && p.held != BTN_NONE && is_repeatable(p.held) && p.hover == p.held &&
                   now >= p.next_repeat_us) {
            actions |= apply(p.held, ctx, now);
            p.next_repeat_us = now + REPEAT_US;
        }
    }
    // DualShock 4 right stick: up shows the rows above.
    const float stick = rays[SETTINGS_RAY_PAD].scroll;
    if (is_settings() && fabsf(stick) > 0.25f) {
        g.scroll = clamp_scroll(g.scroll - stick * SCROLL_STICK_M_PER_S * dt);
    }
    // The pointer loop above tested the rays against the panel as it was at its start.
    if (calib && g.calib)
        actions |= calibration_update(rays, pressed_now, ctx, now);
    // Wizard without any controller to click with (PS Moves and the DualShock 4 are
    // optional): it goes on by itself after 20 s with the default height.
    int no_move_left_s = -1;
    // Also from its calibration (Cancel cannot be clicked either without a controller).
    if (g.wizard && !(actions & SETTINGS_CONFIRMED)) {
        if (ctx.pointers_connected > 0) {
            g.no_move_since = 0;
        } else {
            if (!g.no_move_since)
                g.no_move_since = now;
            if (now - g.no_move_since >= WIZARD_NO_MOVE_US) {
                LOG("settings: no controller connected, the first launch wizard goes on by itself");
                if (g.calib)
                    apply(BTN_CANCEL, ctx, now);
                actions |= apply(BTN_CONFIRM, ctx, now);
            } else {
                no_move_left_s = (int)((WIZARD_NO_MOVE_US - (now - g.no_move_since)) / 1000000) + 1;
            }
        }
    }
    const ClientConfig *c = ctx.config;
    if (c->camera_height_cm != before.camera_height_cm || c->user_height_cm != before.user_height_cm ||
        c->head_prediction_percent != before.head_prediction_percent ||
        c->controller_prediction_ms != before.controller_prediction_ms ||
        c->resolution_percent != before.resolution_percent || c->center_on_connect != before.center_on_connect ||
        c->refresh_rate != before.refresh_rate || c->vibration_percent != before.vibration_percent ||
        c->hud != before.hud) {
        g.dirty = true;
        g.last_change_us = now;
    }
    if (g.dirty && (now - g.last_change_us >= SAVE_DELAY_US || (actions & (SETTINGS_RESET | SETTINGS_CONFIRMED))))
        save_now(c);
    if (g.reset_armed_us && now - g.reset_armed_us > RESET_CONFIRM_US)
        g.reset_armed_us = 0;

    // Values shown. The floor in use changes as soon as the height does.
    const float floor_y = c->camera_height_cm > 0 ? -c->camera_height_cm / 100.0f : ctx.floor_y;
    // The height shown follows the headset above that floor (smoothed; the wizard shows the
    // default until the height is set).
    if (!g.head_smooth_valid) {
        g.head_smooth = ctx.head_y;
        g.head_smooth_valid = true;
    } else {
        const float k = dt / 0.3f;
        g.head_smooth += (ctx.head_y - g.head_smooth) * (k > 1.0f ? 1.0f : k);
    }
    if (c->camera_height_cm > 0 && (!g.wizard || g.height_touched))
        g.shown_height_cm = clamp_height_cm((g.head_smooth - floor_y) / EYE_HEIGHT_RATIO * 100.0f);
    else
        g.shown_height_cm = c->user_height_cm > 0 ? c->user_height_cm : DEFAULT_USER_HEIGHT_CM;
    format_height(g.user, sizeof(g.user), g.shown_height_cm / 100.0f);
    format_height(g.camera, sizeof(g.camera), -floor_y);
    if (g.result_until && now < g.result_until)
        snprintf(g.tip, sizeof(g.tip), "%s", g.result);
    else if (g.wizard && no_move_left_s > 0)
        snprintf(g.tip, sizeof(g.tip), "No controller connected: going on with this height in %d s", no_move_left_s);
    else if (g.wizard)
        snprintf(g.tip, sizeof(g.tip), "%s",
                 "Point with a PS Move (trigger) or the DualShock 4 (Cross). You can change it later.");
    else
        snprintf(g.tip, sizeof(g.tip), "%s",
                 c->user_height_cm > 0 ? "It follows your headset: stand straight, then - / + move the floor"
                                       : "Floor estimated: stand straight and set your height, or calibrate it");
    if (g.hint_until && now >= g.hint_until)
        g.hint_until = 0;
    if (c->head_prediction_percent)
        snprintf(g.hpred, sizeof(g.hpred), "%d %%  of the latency", c->head_prediction_percent);
    else
        snprintf(g.hpred, sizeof(g.hpred), "Off");
    snprintf(g.pred, sizeof(g.pred), "%d ms", c->controller_prediction_ms);
    snprintf(g.res, sizeof(g.res), "%d %%", c->resolution_percent);
    snprintf(g.rate, sizeof(g.rate), "%d Hz", c->refresh_rate);

    if (c->vibration_percent)
        snprintf(g.vibration, sizeof(g.vibration), "%d %%", c->vibration_percent);
    else
        snprintf(g.vibration, sizeof(g.vibration), "Off");
    if (close || (actions & (SETTINGS_CONFIRMED | SETTINGS_RESET)))
        close_panel();
    return actions;
}

static LobbyPanelItem *add(LobbyPanel *p, float x0, float y0, float x1, float y1)
{
    if (p->count >= LOBBY_PANEL_MAX_ITEMS)
        return nullptr;
    LobbyPanelItem *it = &p->items[p->count++];
    memset(it, 0, sizeof(*it));
    it->x0 = x0;
    it->y0 = y0;
    it->x1 = x1;
    it->y1 = y1;
    return it;
}

static void text(LobbyPanel *p, float x0, float x1, float row, const char *s, float h, int align, uint32_t rgb)
{
    LobbyPanelItem *it = add(p, x0, row - h, x1, row + h);
    if (!it)
        return;
    it->text = s;
    it->text_h = h;
    it->align = align;
    it->text_rgb = rgb;
}

// Text on a scrolled row of the settings (hidden when the row is out of the viewport).
static void row_text(LobbyPanel *p, float x0, float x1, float row, const char *s, float h, int align, uint32_t rgb)
{
    if (row_visible(row, h))
        text(p, x0, x1, row + g.scroll, s, h, align, rgb);
}

static void shape(LobbyPanel *p, float x0, float y0, float x1, float y1, uint32_t rgb)
{
    if (p->shape_count >= LOBBY_PANEL_MAX_SHAPES)
        return;
    p->shapes[p->shape_count++] = LobbyPanelShape{x0, y0, x1, y1, 0.0f, rgb};
}

static void circle(LobbyPanel *p, float x, float y, float r, uint32_t rgb)
{
    if (p->shape_count >= LOBBY_PANEL_MAX_SHAPES)
        return;
    p->shapes[p->shape_count++] = LobbyPanelShape{x, y, 0.0f, 0.0f, r, rgb};
}

static const uint32_t FIGURE_RGB = 0xc0c8d0, CONTROLLER_RGB = 0x60a0ff, GROUND_RGB = 0x506070;

// PS Move held upright in a fist at (x, y): sphere above, handle down.
static void picto_move(LobbyPanel *p, float x, float y)
{
    circle(p, x, y + 0.019f, 0.010f, CONTROLLER_RGB);
    shape(p, x, y + 0.009f, x, y - 0.022f, CONTROLLER_RGB);
}

// T-pose: standing, arms straight out to the sides, a PS Move in each hand; arrows outwards.
static void picto_tpose(LobbyPanel *p, float cx)
{
    const float g = CROW_GROUND, hip = g + 0.085f, neck = g + 0.160f, arm = g + 0.150f, hand = 0.150f;
    shape(p, cx - 0.19f, g, cx + 0.19f, g, GROUND_RGB);
    circle(p, cx, neck + 0.021f, 0.018f, FIGURE_RGB);
    shape(p, cx, neck, cx, hip, FIGURE_RGB);
    shape(p, cx, hip, cx - 0.032f, g, FIGURE_RGB);
    shape(p, cx, hip, cx + 0.032f, g, FIGURE_RGB);
    shape(p, cx - hand, arm, cx + hand, arm, FIGURE_RGB);
    picto_move(p, cx - hand - 0.006f, arm);
    picto_move(p, cx + hand + 0.006f, arm);
    for (int s = -1; s <= 1; s += 2) { // arrows under the arms
        const float x0 = cx + s * 0.050f, x1 = cx + s * 0.120f, y = arm - 0.035f;
        shape(p, x0, y, x1, y, STATUS_WAIT_RGB);
        shape(p, x1, y, x1 - s * 0.012f, y + 0.008f, STATUS_WAIT_RGB);
        shape(p, x1, y, x1 - s * 0.012f, y - 0.008f, STATUS_WAIT_RGB);
    }
}

// Crouching, one arm reaching down, the PS Move's ball touching the floor.
static void picto_floor(LobbyPanel *p, float cx)
{
    const float g = CROW_GROUND;
    shape(p, cx - 0.19f, g, cx + 0.19f, g, GROUND_RGB);
    const float hip_x = cx + 0.085f, hip_y = g + 0.060f, knee_x = cx + 0.015f, knee_y = g + 0.075f;
    const float neck_x = cx + 0.030f, neck_y = g + 0.125f;
    circle(p, neck_x - 0.012f, neck_y + 0.020f, 0.018f, FIGURE_RGB);
    shape(p, neck_x, neck_y, hip_x, hip_y, FIGURE_RGB);          // back
    shape(p, hip_x, hip_y, knee_x, knee_y, FIGURE_RGB);          // thigh
    shape(p, knee_x, knee_y, cx + 0.045f, g, FIGURE_RGB);        // shin
    shape(p, cx + 0.045f, g, cx + 0.015f, g, FIGURE_RGB);        // foot
    shape(p, hip_x, hip_y, cx + 0.125f, g + 0.012f, FIGURE_RGB); // other leg, knee down
    shape(p, cx + 0.125f, g + 0.012f, cx + 0.165f, g, FIGURE_RGB);
    const float hand_x = cx - 0.070f, hand_y = g + 0.040f;
    shape(p, neck_x - 0.002f, neck_y - 0.010f, hand_x, hand_y, FIGURE_RGB); // arm
    // The Move upside down in the hand, its ball on the floor, with contact marks.
    const float ball_x = hand_x - 0.004f, ball_y = g + 0.010f;
    circle(p, ball_x, ball_y, 0.010f, CONTROLLER_RGB);
    shape(p, ball_x, ball_y + 0.010f, hand_x + 0.002f, hand_y + 0.012f, CONTROLLER_RGB);
    for (int s = -1; s <= 1; s += 2) {
        shape(p, ball_x + s * 0.016f, g + 0.004f, ball_x + s * 0.030f, g + 0.012f, STATUS_WAIT_RGB);
        shape(p, ball_x + s * 0.018f, g - 0.004f, ball_x + s * 0.032f, g - 0.004f, STATUS_WAIT_RGB);
    }
}

void settings_build(LobbyPanel *panel, LobbyPointer pointers[LOBBY_POINTERS])
{
    memset(pointers, 0, sizeof(LobbyPointer) * LOBBY_POINTERS);
    panel->visible = g.open;
    panel->count = 0;
    panel->shape_count = 0;
    if (!g.open)
        return;
    panel->origin = g.origin;
    panel->right = v3(1, 0, 0);
    panel->up = v3(0, 1, 0);
    float half_w, top, bottom;
    panel_bounds(&half_w, &top, &bottom);
    const float w = half_w * 2;
    LobbyPanelItem *bg = add(panel, -half_w, bottom, half_w, top);
    bg->fill = 0x0b0f16;
    bg->outline = 0x4080c0;

    const float lx0 = -w / 2 + 0.01f;
    if (g.calib) {
        text(panel, -half_w, half_w, CROW_TITLE, g.surface_test ? "Surface test (Dev)" : "Height calibration", TITLE_H, 0,
             0xffffff);
        text(panel, -half_w, half_w, CROW_TEXT,
             g.surface_test ? "On a table the camera sees: a PS Move ball down, then lying; pull its trigger"
                            : "Do one of the two",
             LABEL_H * 0.8f, 0, 0xc0c8d0);
        const float cap_h = TIP_H * 1.15f;
        for (int side = -1; side <= 1; side += 2) {
            const bool tpose = side < 0; // T-pose on the left, floor on the right
            const float x0 = side < 0 ? -CBOX_X1 : CBOX_X0, x1 = side < 0 ? -CBOX_X0 : CBOX_X1;
            const bool ready = tpose ? g.tpose_ready : g.floor_ready;
            LobbyPanelItem *box = add(panel, x0, CBOX_BOTTOM, x1, CBOX_TOP);
            if (box) {
                box->fill = 0x111722;
                box->outline = ready && !g.surface_test ? STATUS_READY_RGB : 0x34445a;
            }
            if (tpose)
                picto_tpose(panel, (x0 + x1) / 2);
            else
                picto_floor(panel, (x0 + x1) / 2);
            text(panel, x0, x1, CROW_CAPTION1, tpose ? "Arms out to the sides, PS Moves up," : "Touch the floor with a PS Move's ball,",
                 cap_h, 0, 0xffffff);
            text(panel, x0, x1, CROW_CAPTION2, tpose ? "pull both triggers" : "pull its trigger", cap_h, 0, 0xffffff);
            text(panel, x0, x1, CROW_STATUS, tpose ? g.tpose_status : g.floor_status, cap_h, 0,
                 tpose ? g.tpose_rgb : g.floor_rgb);
        }
        if (g.hint_until)
            text(panel, -half_w, half_w, CROW_HINT, g.hint, TIP_H * 1.15f, 0,
                 g.surface_test ? STATUS_READY_RGB : STATUS_WARN_RGB);
    } else if (g.wizard) {
        const float dx = -(PANEL_W - WIZARD_W) / 2, vx0 = -0.14f + dx, vx1 = MINUS_X + dx - 0.01f;
        text(panel, -w / 2, w / 2, WROW_TITLE, "Welcome to ALVR PS4", TITLE_H, 0, 0xffffff);
        text(panel, -w / 2, w / 2, WROW_TEXT, "Stand straight, set or measure your height, then confirm",
             LABEL_H * 0.8f, 0, 0xc0c8d0);
        text(panel, lx0, vx0, WROW_USER, "Your height", LABEL_H, -1, 0xc0c8d0);
        text(panel, vx0, vx1, WROW_USER, g.user, LABEL_H, -1, 0xffffff);
        text(panel, lx0, vx1, WROW_CALIB, "Or measure it (floor or arm span)", LABEL_H * 0.8f, -1, 0x8090a0);
        text(panel, lx0, w / 2, WROW_TIP, g.tip, TIP_H, 0, 0x8090a0);
    } else {
        const float vx0 = -0.14f, vx1 = MINUS_X - 0.01f;
        text(panel, -w / 2, w / 2, ROW_TITLE, "Settings", TITLE_H, 0, 0xffffff);
        row_text(panel, lx0, vx0, ROW_USER, "Your height", LABEL_H, -1, 0xc0c8d0);
        row_text(panel, vx0, vx1, ROW_USER, g.user, LABEL_H, -1, 0xffffff);
        row_text(panel, lx0, vx0, ROW_CAMERA, "PS Camera height", LABEL_H, -1, 0x8090a0);
        row_text(panel, vx0, vx1, ROW_CAMERA, g.camera, LABEL_H, -1, 0xa0a8b0);
        row_text(panel, lx0, w / 2, ROW_TIP, g.tip, TIP_H, -1, 0x8090a0);
        row_text(panel, lx0, vx0, ROW_HPRED, "Headset prediction", LABEL_H, -1, 0xc0c8d0);
        row_text(panel, vx0, vx1, ROW_HPRED, g.hpred, LABEL_H, -1, 0xffffff);
        row_text(panel, lx0, vx0, ROW_PRED, "Controller prediction", LABEL_H, -1, 0xc0c8d0);
        row_text(panel, vx0, vx1, ROW_PRED, g.pred, LABEL_H, -1, 0xffffff);
        row_text(panel, lx0, vx0, ROW_RES, "Stream resolution", LABEL_H, -1, 0xc0c8d0);
        const bool res_changed = g_cfg && g_cfg->resolution_percent != g_launch_resolution;
        const bool rate_changed = g_cfg && g_cfg->refresh_rate != g_launch_refresh_rate;
        row_text(panel, vx0, vx1, ROW_RES, g.res, LABEL_H, -1, 0xffffff);
        const float rx0 = vx0 + 0.125f, rh = LABEL_H * 0.85f; // "(restart required)", after the value
        row_text(panel, rx0, vx1, ROW_RES, "(restart required)", rh, -1, res_changed ? RESTART_CHANGED_RGB : RESTART_RGB);
        row_text(panel, lx0, vx0, ROW_RATE, "Refresh rate", LABEL_H, -1, 0xc0c8d0);
        row_text(panel, rx0, vx1, ROW_RATE, "(restart required)", rh, -1,
                 rate_changed ? RESTART_CHANGED_RGB : RESTART_RGB);
        row_text(panel, lx0, MINUS_X - 0.01f, ROW_CENTER, "Center on the headset at SteamVR start", LABEL_H, -1,
                 0xc0c8d0);
        row_text(panel, lx0, vx0, ROW_VIBRATION, "Vibration", LABEL_H, -1, 0xc0c8d0);
        row_text(panel, vx0 - 0.02f, SLIDER_X0 - 0.02f, ROW_VIBRATION, g.vibration, LABEL_H, -1, 0xffffff);
        row_text(panel, lx0, MINUS_X - 0.01f, ROW_HUD, "Performance overlay (in the stream)", LABEL_H, -1, 0xc0c8d0);
        // Scroll bar on the right edge: the viewport's share of the rows, highlighted under
        // the laser or while grabbed.
        bool bar_hot = false;
        for (const Pointer &p : g.ptr)
            bar_hot = bar_hot || (p.dragging && p.bar) || (!p.dragging && p.hit && p.hover == BTN_NONE && on_bar(p.px, p.py));
        const float top = thumb_top(g.scroll);
        if (LobbyPanelItem *t = add(panel, BAR_X0, VIEW_BOTTOM, BAR_X1, VIEW_TOP))
            t->fill = 0x1a2230;
        if (LobbyPanelItem *t = add(panel, BAR_X0, top - BAR_THUMB_H, BAR_X1, top))
            t->fill = bar_hot ? 0xffd040 : 0x5a6c84;
    }

    static const char *labels[BTN_COUNT] = {"-", "+", "-", "+", "-", "+", "-", "+", "", "On", "Close", "Reset settings",
                                            "Confirm", "Calibrate", "Cancel", "", "Off", "Surface test"};
    labels[BTN_RATE] = g.rate;
    labels[BTN_RESET] = g.reset_armed_us ? "Click again to reset" : "Reset settings";
    labels[BTN_CENTER] = g_cfg && !g_cfg->center_on_connect ? "Off" : "On";
    labels[BTN_HUD] = g_cfg && g_cfg->hud ? "On" : "Off";
    labels[BTN_CANCEL] = g.surface_test ? "Close" : "Cancel";
    for (int b = 0; b < BTN_COUNT; b++) {
        if (!button_active(b))
            continue;
        bool hover = false, down = false;
        for (const Pointer &p : g.ptr) {
            hover = hover || p.hover == b;
            down = down || (p.held == b && p.hover == b);
        }
        Rect r = button_rect(b);
        if (b == BTN_VIBRATION) {
            // Slider: a track, its filled part and a knob at the value.
            const float y = (r.y0 + r.y1) / 2, f = g_cfg ? g_cfg->vibration_percent / 100.0f : 1.0f;
            const float kx = SLIDER_X0 + f * (SLIDER_X1 - SLIDER_X0);
            if (LobbyPanelItem *t = add(panel, SLIDER_X0, y - 0.004f, SLIDER_X1, y + 0.004f))
                t->fill = 0x263044;
            if (f > 0.0f)
                if (LobbyPanelItem *t = add(panel, SLIDER_X0, y - 0.004f, kx, y + 0.004f))
                    t->fill = 0x4f86c0;
            if (LobbyPanelItem *k = add(panel, kx - 0.012f, y - 0.020f, kx + 0.012f, y + 0.020f)) {
                k->fill = hover || down ? 0xffd040 : 0xc0c8d0;
                k->outline = 0x0b0f16;
            }
            continue;
        }
        LobbyPanelItem *it = add(panel, r.x0, r.y0, r.x1, r.y1);
        if (!it)
            break;
        const bool armed = b == BTN_RESET && g.reset_armed_us;
        const bool on = (b == BTN_CENTER && g_cfg && g_cfg->center_on_connect) || (b == BTN_HUD && g_cfg && g_cfg->hud) ||
                        (b == BTN_SURFACE && g.surface_test);
        it->fill = down ? 0x4a5670 : armed ? 0x5a1c1c : on ? 0x1c4a30 : hover ? 0x263044 : 0x161c28;
        it->outline = hover ? 0xffd040 : b == BTN_RESET ? 0xc05050 : b == BTN_CONFIRM ? 0x60c080 : 0x7088a0;
        it->text = labels[b];
        it->text_h = b >= BTN_RATE ? (b == BTN_SURFACE ? LABEL_H * 0.8f : LABEL_H) : LABEL_H * 1.3f;
        it->align = 0;
        it->text_rgb = 0xffffff;
    }

    for (int i = 0; i < LOBBY_POINTERS; i++) {
        const Pointer &p = g.ptr[i];
        pointers[i].visible = p.valid;
        pointers[i].from = p.from;
        pointers[i].to = p.to;
        pointers[i].hit = p.hit;
        pointers[i].rgb = p.hover != BTN_NONE || p.dragging ? 0xffd040 : p.hit ? 0xa0d0ff : 0x5080b0;
    }
}
