#include "settings.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "log.h"

// Panel layout in metres (panel coordinates: x right, y up, origin at the centre).
static const float PANEL_W = 1.02f, PANEL_TOP = 0.31f, PANEL_BOTTOM = -0.53f;
static const float WIZARD_W = 0.90f, WIZARD_TOP = 0.19f, WIZARD_BOTTOM = -0.21f;
static const float LABEL_H = 0.022f, TITLE_H = 0.032f, TIP_H = 0.0135f;
static const float BTN_W = 0.07f, BTN_H = 0.05f;
static const float MINUS_X = 0.320f, PLUS_X = 0.405f; // left edges of the - / + buttons
// Trigger thresholds for a click, with hysteresis; held +/- buttons repeat.
static const float TRIGGER_PRESS = 0.55f, TRIGGER_RELEASE = 0.35f;
static const uint64_t REPEAT_DELAY_US = 450000, REPEAT_US = 50000;
static const uint64_t SAVE_DELAY_US = 2000000; // config written 2 s after the last change

enum Button { BTN_NONE = -1, BTN_HEIGHT_MINUS, BTN_HEIGHT_PLUS, BTN_PRED_MINUS, BTN_PRED_PLUS, BTN_RES_MINUS,
              BTN_RES_PLUS, BTN_RATE, BTN_CENTER, BTN_CLOSE, BTN_RESET, BTN_CONFIRM, BTN_COUNT };

struct Rect {
    float x0, y0, x1, y1;
};

// Row centres of the settings panel.
static const float ROW_TITLE = 0.255f, ROW_USER = 0.165f, ROW_CAMERA = 0.095f, ROW_TIP = 0.045f,
                   ROW_PRED = -0.035f, ROW_RES = -0.115f, ROW_RATE = -0.195f, ROW_CENTER = -0.275f,
                   ROW_ACTIONS = -0.375f, ROW_RESET = -0.470f;
// Row centres of the first launch wizard.
static const float WROW_TITLE = 0.135f, WROW_TEXT = 0.075f, WROW_USER = 0.000f, WROW_TIP = -0.060f,
                   WROW_CONFIRM = -0.140f;
static const uint64_t RESET_CONFIRM_US = 3000000; // second click within 3 s confirms
static const uint64_t WIZARD_NO_MOVE_US = 20000000; // no PS Move to click with: go on after 20 s
static const int DEFAULT_USER_HEIGHT_CM = SETTINGS_DEFAULT_USER_HEIGHT_CM;
static const float EYE_HEIGHT_RATIO = SETTINGS_EYE_HEIGHT_RATIO;

struct Pointer {
    bool valid, hit;
    Vec3 from, to;
    int hover; // Button under the laser
    bool pressed;
    int held;  // button being held (+/- repeat)
    uint64_t next_repeat_us;
};

static struct {
    bool open;
    bool wizard;             // first launch: height and Confirm only, cannot be closed
    uint64_t no_move_since;  // wizard without any PS Move connected, since then (0: one is on)
    Vec3 origin;
    Pointer ptr[LOBBY_POINTERS];
    bool dirty;
    uint64_t last_change_us;
    uint64_t reset_armed_us; // first Reset click, waiting for the confirmation
    // Values shown, refreshed by settings_update.
    char user[48], camera[48], tip[96], pred[32], res[48], rate[16];
} g;

static bool button_active(int b)
{
    if (g.wizard)
        return b == BTN_HEIGHT_MINUS || b == BTN_HEIGHT_PLUS || b == BTN_CONFIRM;
    return b != BTN_CONFIRM;
}

static Rect button_rect(int b)
{
    auto pm = [](float x, float row) { return Rect{x, row - BTN_H / 2, x + BTN_W, row + BTN_H / 2}; };
    if (g.wizard) {
        // The - / + pair sits at the same x as in the settings, shifted into the narrower panel.
        const float dx = -(PANEL_W - WIZARD_W) / 2;
        switch (b) {
        case BTN_HEIGHT_MINUS: return pm(MINUS_X + dx, WROW_USER);
        case BTN_HEIGHT_PLUS: return pm(PLUS_X + dx, WROW_USER);
        case BTN_CONFIRM: return Rect{-0.15f, WROW_CONFIRM - 0.03f, 0.15f, WROW_CONFIRM + 0.03f};
        }
        return Rect{0, 0, 0, 0};
    }
    switch (b) {
    case BTN_HEIGHT_MINUS: return pm(MINUS_X, ROW_USER);
    case BTN_HEIGHT_PLUS: return pm(PLUS_X, ROW_USER);
    case BTN_PRED_MINUS: return pm(MINUS_X, ROW_PRED);
    case BTN_PRED_PLUS: return pm(PLUS_X, ROW_PRED);
    case BTN_RES_MINUS: return pm(MINUS_X, ROW_RES);
    case BTN_RES_PLUS: return pm(PLUS_X, ROW_RES);
    case BTN_RATE: return Rect{MINUS_X, ROW_RATE - BTN_H / 2, PLUS_X + BTN_W, ROW_RATE + BTN_H / 2};
    case BTN_CENTER: return Rect{MINUS_X, ROW_CENTER - BTN_H / 2, PLUS_X + BTN_W, ROW_CENTER + BTN_H / 2};
    case BTN_CLOSE: return Rect{-0.15f, ROW_ACTIONS - 0.03f, 0.15f, ROW_ACTIONS + 0.03f};
    case BTN_RESET: return Rect{-0.19f, ROW_RESET - 0.03f, 0.19f, ROW_RESET + 0.03f};
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
    LOG("settings: saved (user height %d cm, camera height %d cm, prediction %d ms, resolution %d%%, %d Hz, "
        "center on SteamVR start %d)",
        cfg->user_height_cm, cfg->camera_height_cm, cfg->controller_prediction_ms, cfg->resolution_percent,
        cfg->refresh_rate, cfg->center_on_connect);
}

static ClientConfig *g_cfg; // last config seen, to save on close

static void open_panel(Vec3 head_pos, bool wizard)
{
    g.open = true;
    g.wizard = wizard;
    g.no_move_since = 0;
    // In front of the head towards the camera (-Z), slightly below eye level; the head
    // orientation is ignored so the panel always faces the play area's forward.
    g.origin = head_pos + v3(0.0f, -0.12f, -0.85f);
    for (Pointer &p : g.ptr) {
        p.pressed = true; // wait for a trigger release first
        p.held = BTN_NONE;
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

// The user stands straight: the floor goes below the headset at the eye height of their
// height. The camera is the tracker origin, so the floor is kept as the camera height,
// which stays valid after tracking resets.
static void place_floor(ClientConfig *c, const SettingsContext &ctx)
{
    const int cm = (int)lroundf((c->user_height_cm * EYE_HEIGHT_RATIO / 100.0f - ctx.head_y) * 100.0f);
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
        c->camera_height_cm = 0;
        c->user_height_cm = 0;
        c->center_on_connect = 1;
        c->refresh_rate = 90;
        LOG("settings: reset to defaults");
        return SETTINGS_RESET;
    case BTN_HEIGHT_MINUS:
    case BTN_HEIGHT_PLUS: {
        int h = c->user_height_cm > 0 ? c->user_height_cm : DEFAULT_USER_HEIGHT_CM;
        h += b == BTN_HEIGHT_PLUS ? 1 : -1;
        c->user_height_cm = h < 100 ? 100 : h > 230 ? 230 : h;
        place_floor(c, ctx);
        return SETTINGS_HEIGHT_CHANGED;
    }
    case BTN_CONFIRM:
        // The floor comes from the headset height at this moment (standing straight).
        if (c->user_height_cm <= 0)
            c->user_height_cm = DEFAULT_USER_HEIGHT_CM;
        place_floor(c, ctx);
        LOG("settings: first launch height confirmed: %d cm (camera height %d cm)", c->user_height_cm,
            c->camera_height_cm);
        return SETTINGS_HEIGHT_CHANGED | SETTINGS_CONFIRMED;
    case BTN_CENTER:
        c->center_on_connect = !c->center_on_connect;
        return 0;
    case BTN_RATE:
        c->refresh_rate = c->refresh_rate == 90 ? 60 : 90;
        return 0;
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

unsigned settings_update(const SettingsContext &ctx, const SettingsRay rays[LOBBY_POINTERS], uint64_t now,
                         int *clicked_hand)
{
    *clicked_hand = -1;
    g_cfg = ctx.config;
    if (!g.open)
        return 0;
    unsigned actions = 0;
    const ClientConfig before = *ctx.config;
    bool close = false;
    const float half_w = (g.wizard ? WIZARD_W : PANEL_W) / 2, top = g.wizard ? WIZARD_TOP : PANEL_TOP,
                bottom = g.wizard ? WIZARD_BOTTOM : PANEL_BOTTOM;
    for (int i = 0; i < LOBBY_POINTERS; i++) {
        Pointer &p = g.ptr[i];
        const SettingsRay &r = rays[i];
        p.valid = r.valid;
        p.hit = false;
        p.hover = BTN_NONE;
        if (!r.valid)
            continue;
        p.from = r.origin;
        p.to = r.origin + r.dir * 1.5f;
        // The panel faces +Z: intersect the plane z = origin.z.
        if (r.dir.z < -1e-3f) {
            float t = (g.origin.z - r.origin.z) / r.dir.z;
            if (t > 0.0f) {
                Vec3 hit = r.origin + r.dir * t;
                float px = hit.x - g.origin.x, py = hit.y - g.origin.y;
                if (px >= -half_w && px <= half_w && py >= bottom && py <= top) {
                    p.hit = true;
                    p.to = hit;
                    for (int b = 0; b < BTN_COUNT; b++) {
                        if (!button_active(b))
                            continue;
                        Rect rc = button_rect(b);
                        if (px >= rc.x0 && px <= rc.x1 && py >= rc.y0 && py <= rc.y1)
                            p.hover = b;
                    }
                }
            }
        }
        // Trigger: a click on press, then repeats while held on a +/- button.
        if (!p.pressed && r.trigger >= TRIGGER_PRESS) {
            p.pressed = true;
            p.held = p.hover;
            if (p.hover != BTN_NONE) {
                *clicked_hand = i;
                if (p.hover == BTN_CLOSE)
                    close = true;
                else
                    actions |= apply(p.hover, ctx, now);
                p.next_repeat_us = now + REPEAT_DELAY_US;
            }
        } else if (p.pressed && r.trigger <= TRIGGER_RELEASE) {
            p.pressed = false;
            p.held = BTN_NONE;
        } else if (p.pressed && p.held != BTN_NONE && is_repeatable(p.held) && p.hover == p.held &&
                   now >= p.next_repeat_us) {
            actions |= apply(p.held, ctx, now);
            p.next_repeat_us = now + REPEAT_US;
        }
    }
    // Wizard without any controller to click with (PS Moves and the DualShock 4 are
    // optional): it goes on by itself after 20 s with the default height.
    int no_move_left_s = -1;
    if (g.wizard && !(actions & SETTINGS_CONFIRMED)) {
        if (ctx.pointers_connected > 0) {
            g.no_move_since = 0;
        } else {
            if (!g.no_move_since)
                g.no_move_since = now;
            if (now - g.no_move_since >= WIZARD_NO_MOVE_US) {
                LOG("settings: no controller connected, the first launch wizard goes on by itself");
                actions |= apply(BTN_CONFIRM, ctx, now);
            } else {
                no_move_left_s = (int)((WIZARD_NO_MOVE_US - (now - g.no_move_since)) / 1000000) + 1;
            }
        }
    }
    const ClientConfig *c = ctx.config;
    if (c->camera_height_cm != before.camera_height_cm || c->user_height_cm != before.user_height_cm ||
        c->controller_prediction_ms != before.controller_prediction_ms ||
        c->resolution_percent != before.resolution_percent || c->center_on_connect != before.center_on_connect ||
        c->refresh_rate != before.refresh_rate) {
        g.dirty = true;
        g.last_change_us = now;
    }
    if (g.dirty && (now - g.last_change_us >= SAVE_DELAY_US || (actions & (SETTINGS_RESET | SETTINGS_CONFIRMED))))
        save_now(c);
    if (g.reset_armed_us && now - g.reset_armed_us > RESET_CONFIRM_US)
        g.reset_armed_us = 0;

    // Values shown. The floor in use changes as soon as the height does.
    const float floor_y = c->camera_height_cm > 0 ? -c->camera_height_cm / 100.0f : ctx.floor_y;
    format_height(g.user, sizeof(g.user), (c->user_height_cm > 0 ? c->user_height_cm : DEFAULT_USER_HEIGHT_CM) / 100.0f);
    format_height(g.camera, sizeof(g.camera), -floor_y);
    if (g.wizard && no_move_left_s > 0)
        snprintf(g.tip, sizeof(g.tip), "No controller connected: going on with this height in %d s", no_move_left_s);
    else if (g.wizard)
        snprintf(g.tip, sizeof(g.tip), "%s",
                 "Point with a PS Move (trigger) or the DualShock 4 (Cross). You can change it later.");
    else
        snprintf(g.tip, sizeof(g.tip), "%s",
                 c->user_height_cm > 0 ? "Stand straight when you change it: the floor is placed from your headset"
                                       : "Floor estimated: stand straight and set your height with - / +");
    snprintf(g.pred, sizeof(g.pred), "%d ms", c->controller_prediction_ms);
    snprintf(g.res, sizeof(g.res), "%d %%  (next launch)", c->resolution_percent);
    snprintf(g.rate, sizeof(g.rate), "%d Hz", c->refresh_rate);
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

void settings_build(LobbyPanel *panel, LobbyPointer pointers[LOBBY_POINTERS])
{
    memset(pointers, 0, sizeof(LobbyPointer) * LOBBY_POINTERS);
    panel->visible = g.open;
    panel->count = 0;
    if (!g.open)
        return;
    panel->origin = g.origin;
    panel->right = v3(1, 0, 0);
    panel->up = v3(0, 1, 0);
    const float w = g.wizard ? WIZARD_W : PANEL_W;
    LobbyPanelItem *bg = add(panel, -w / 2, g.wizard ? WIZARD_BOTTOM : PANEL_BOTTOM, w / 2,
                             g.wizard ? WIZARD_TOP : PANEL_TOP);
    bg->fill = 0x0b0f16;
    bg->outline = 0x4080c0;

    const float lx0 = -w / 2 + 0.01f;
    if (g.wizard) {
        const float dx = -(PANEL_W - WIZARD_W) / 2, vx0 = -0.14f + dx, vx1 = MINUS_X + dx - 0.01f;
        text(panel, -w / 2, w / 2, WROW_TITLE, "Welcome to ALVR PS4", TITLE_H, 0, 0xffffff);
        text(panel, -w / 2, w / 2, WROW_TEXT, "Stand straight, set your height, then confirm", LABEL_H * 0.8f, 0,
             0xc0c8d0);
        text(panel, lx0, vx0, WROW_USER, "Your height", LABEL_H, -1, 0xc0c8d0);
        text(panel, vx0, vx1, WROW_USER, g.user, LABEL_H, -1, 0xffffff);
        text(panel, lx0, w / 2, WROW_TIP, g.tip, TIP_H, 0, 0x8090a0);
    } else {
        const float vx0 = -0.14f, vx1 = MINUS_X - 0.01f;
        text(panel, -w / 2, w / 2, ROW_TITLE, "Settings", TITLE_H, 0, 0xffffff);
        text(panel, lx0, vx0, ROW_USER, "Your height", LABEL_H, -1, 0xc0c8d0);
        text(panel, vx0, vx1, ROW_USER, g.user, LABEL_H, -1, 0xffffff);
        text(panel, lx0, vx0, ROW_CAMERA, "PS Camera height", LABEL_H, -1, 0x8090a0);
        text(panel, vx0, vx1, ROW_CAMERA, g.camera, LABEL_H, -1, 0xa0a8b0);
        text(panel, lx0, w / 2, ROW_TIP, g.tip, TIP_H, -1, 0x8090a0);
        text(panel, lx0, vx0, ROW_PRED, "Controller prediction", LABEL_H, -1, 0xc0c8d0);
        text(panel, vx0, vx1, ROW_PRED, g.pred, LABEL_H, -1, 0xffffff);
        text(panel, lx0, vx0, ROW_RES, "Stream resolution", LABEL_H, -1, 0xc0c8d0);
        text(panel, vx0, vx1, ROW_RES, g.res, LABEL_H, -1, 0xffffff);
        text(panel, lx0, vx0, ROW_RATE, "Refresh rate", LABEL_H, -1, 0xc0c8d0);
        text(panel, vx0, vx1, ROW_RATE, "(next launch)", LABEL_H, -1, 0xffffff);
        text(panel, lx0, MINUS_X - 0.01f, ROW_CENTER, "Center on the headset at SteamVR start", LABEL_H, -1,
             0xc0c8d0);
    }

    static const char *labels[BTN_COUNT] = {"-", "+", "-", "+", "-", "+", "", "On", "Close", "Reset settings", "Confirm"};
    labels[BTN_RATE] = g.rate;
    labels[BTN_RESET] = g.reset_armed_us ? "Click again to reset" : "Reset settings";
    labels[BTN_CENTER] = g_cfg && !g_cfg->center_on_connect ? "Off" : "On";
    for (int b = 0; b < BTN_COUNT; b++) {
        if (!button_active(b))
            continue;
        bool hover = false, down = false;
        for (const Pointer &p : g.ptr) {
            hover = hover || p.hover == b;
            down = down || (p.held == b && p.hover == b);
        }
        Rect r = button_rect(b);
        LobbyPanelItem *it = add(panel, r.x0, r.y0, r.x1, r.y1);
        if (!it)
            break;
        const bool armed = b == BTN_RESET && g.reset_armed_us;
        const bool on = b == BTN_CENTER && g_cfg && g_cfg->center_on_connect;
        it->fill = down ? 0x4a5670 : armed ? 0x5a1c1c : on ? 0x1c4a30 : hover ? 0x263044 : 0x161c28;
        it->outline = hover ? 0xffd040 : b == BTN_RESET ? 0xc05050 : b == BTN_CONFIRM ? 0x60c080 : 0x7088a0;
        it->text = labels[b];
        it->text_h = b >= BTN_RATE ? LABEL_H : LABEL_H * 1.3f;
        it->align = 0;
        it->text_rgb = 0xffffff;
    }

    for (int i = 0; i < LOBBY_POINTERS; i++) {
        const Pointer &p = g.ptr[i];
        pointers[i].visible = p.valid;
        pointers[i].from = p.from;
        pointers[i].to = p.to;
        pointers[i].hit = p.hit;
        pointers[i].rgb = p.hover != BTN_NONE ? 0xffd040 : p.hit ? 0xa0d0ff : 0x5080b0;
    }
}
