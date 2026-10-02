#pragma once

// Wire format between the PS4 app and ALVR PS4 Tracking Viewer (the PC companion in
// companion/), compiled into both. The viewer sends a hello to TRACKVIEW_PORT on the console
// about every second; while hellos keep coming, the console sends it back what it tracks (as
// the lobby would draw it, the headset included) at up to 60 Hz. UDP, little-endian (both
// ends are x86-64).

#include <stdint.h>
#include <string.h>

#include "lobby.h"

#define TRACKVIEW_PORT 9955 // 9943 / 9944 are ALVR's, 9950 the Dev build's logs
#define TRACKVIEW_VERSION 1
#define TRACKVIEW_HELLO_MAGIC 0x48345041u // "AP4H"
#define TRACKVIEW_STATE_MAGIC 0x54345041u // "AP4T"
#define TRACKVIEW_HELLO_PERIOD_US 1000000ull
#define TRACKVIEW_TIMEOUT_US 3000000ull    // a viewer without hello for this long is dropped
#define TRACKVIEW_SEND_PERIOD_US 15000ull  // 60 Hz at most
#define TRACKVIEW_PACKET_MAX 1400

struct TrackViewBuf {
    uint8_t *p;
    const uint8_t *end;
    bool ok;
};

static inline void trackview_put(TrackViewBuf &b, const void *v, int n)
{
    if (!b.ok || b.end - b.p < n) {
        b.ok = false;
        return;
    }
    memcpy(b.p, v, n);
    b.p += n;
}

static inline void trackview_get(TrackViewBuf &b, void *v, int n)
{
    if (!b.ok || b.end - b.p < n) {
        b.ok = false;
        memset(v, 0, n);
        return;
    }
    memcpy(v, b.p, n);
    b.p += n;
}

static inline void trackview_put_u8(TrackViewBuf &b, uint8_t v) { trackview_put(b, &v, 1); }
static inline void trackview_put_f32(TrackViewBuf &b, float v) { trackview_put(b, &v, 4); }
static inline void trackview_put_pose(TrackViewBuf &b, Vec3 p, Quat q)
{
    const float f[7] = {p.x, p.y, p.z, q.x, q.y, q.z, q.w};
    trackview_put(b, f, sizeof(f));
}
static inline uint8_t trackview_get_u8(TrackViewBuf &b)
{
    uint8_t v;
    trackview_get(b, &v, 1);
    return v;
}
static inline float trackview_get_f32(TrackViewBuf &b)
{
    float v;
    trackview_get(b, &v, 4);
    return v;
}
static inline void trackview_get_pose(TrackViewBuf &b, Vec3 *p, Quat *q)
{
    float f[7];
    trackview_get(b, f, sizeof(f));
    *p = v3(f[0], f[1], f[2]);
    *q = Quat{f[3], f[4], f[5], f[6]};
}

// Hello from the viewer: magic, version. Returns the size.
static inline int trackview_pack_hello(uint8_t *out, int cap)
{
    TrackViewBuf b{out, out + cap, true};
    const uint32_t magic = TRACKVIEW_HELLO_MAGIC;
    const uint16_t version = TRACKVIEW_VERSION;
    trackview_put(b, &magic, 4);
    trackview_put(b, &version, 2);
    return b.ok ? (int)(b.p - out) : 0;
}

static inline bool trackview_is_hello(const uint8_t *in, int n)
{
    uint32_t magic;
    if (n < 6)
        return false;
    memcpy(&magic, in, 4);
    return magic == TRACKVIEW_HELLO_MAGIC;
}

// State: header (magic, version, flags, sequence number), floor and play space centre, the
// headset, then only the visible controllers and pads (each with its slot in the view).
// Returns the size, 0 if it does not fit.
static inline int trackview_pack_state(const LobbyView *v, uint32_t seq, uint8_t *out, int cap)
{
    TrackViewBuf b{out, out + cap, true};
    const uint32_t magic = TRACKVIEW_STATE_MAGIC;
    const uint16_t version = TRACKVIEW_VERSION, flags = v->grid_visible ? 1 : 0;
    trackview_put(b, &magic, 4);
    trackview_put(b, &version, 2);
    trackview_put(b, &flags, 2);
    trackview_put(b, &seq, 4);
    trackview_put_f32(b, v->floor_y);
    trackview_put_f32(b, v->center_x);
    trackview_put_f32(b, v->center_z);

    const LobbyView::Headset &h = v->headset;
    trackview_put_u8(b, (h.visible ? 1 : 0) | (h.tracked ? 2 : 0));
    trackview_put_pose(b, h.pos, h.rot);

    uint8_t count = 0;
    for (const LobbyView::Controller &c : v->controllers)
        count += c.visible;
    trackview_put_u8(b, count);
    for (int i = 0; i < LOBBY_CONTROLLERS; i++) {
        const LobbyView::Controller &c = v->controllers[i];
        if (!c.visible)
            continue;
        trackview_put_u8(b, (uint8_t)i);
        trackview_put_u8(b, (c.tracked ? 1 : 0) | (c.pad_touch ? 2 : 0) | (c.pad_click ? 4 : 0) | (c.charging ? 8 : 0));
        trackview_put_u8(b, (uint8_t)c.hand_letter);
        trackview_put(b, &c.buttons, 2);
        trackview_put_pose(b, c.pos, c.rot);
        trackview_put(b, &c.rgb, 4);
        trackview_put_f32(b, c.trigger);
        trackview_put_f32(b, c.battery);
        trackview_put_f32(b, c.pad_x);
        trackview_put_f32(b, c.pad_y);
    }

    count = 0;
    for (const LobbyView::Pad &p : v->pads)
        count += p.visible;
    trackview_put_u8(b, count);
    for (int i = 0; i < LOBBY_PADS; i++) {
        const LobbyView::Pad &p = v->pads[i];
        if (!p.visible)
            continue;
        trackview_put_u8(b, (uint8_t)i);
        trackview_put_u8(b, (p.tracked ? 1 : 0) | (p.touch[0] ? 2 : 0) | (p.touch[1] ? 4 : 0) | (p.charging ? 8 : 0) |
                                (p.floating ? 16 : 0));
        trackview_put_u8(b, (uint8_t)p.label);
        trackview_put(b, &p.buttons, 4);
        trackview_put_pose(b, p.pos, p.rot);
        trackview_put(b, &p.rgb, 4);
        const float f[13] = {p.lx,         p.ly,         p.rx,         p.ry,      p.l2,           p.r2,          p.touch_x[0],
                             p.touch_x[1], p.touch_y[0], p.touch_y[1], p.battery, p.rumble_large, p.rumble_small};
        trackview_put(b, f, sizeof(f));
    }
    return b.ok ? (int)(b.p - out) : 0;
}

// Fills the tracked part of the view (the rest is left as it is). False if the packet is not
// a state of this version (*version_out: the version it has, 0 if not a state at all).
static inline bool trackview_unpack_state(const uint8_t *in, int n, LobbyView *v, uint32_t *seq_out,
                                          uint16_t *version_out)
{
    TrackViewBuf b{(uint8_t *)in, in + n, true};
    uint32_t magic, seq;
    uint16_t version, flags;
    trackview_get(b, &magic, 4);
    trackview_get(b, &version, 2);
    *version_out = b.ok && magic == TRACKVIEW_STATE_MAGIC ? version : 0;
    if (*version_out != TRACKVIEW_VERSION)
        return false;
    trackview_get(b, &flags, 2);
    trackview_get(b, &seq, 4);
    v->grid_visible = (flags & 1) != 0;
    v->floor_y = trackview_get_f32(b);
    v->center_x = trackview_get_f32(b);
    v->center_z = trackview_get_f32(b);

    LobbyView::Headset &h = v->headset;
    uint8_t hf = trackview_get_u8(b);
    h.visible = (hf & 1) != 0;
    h.tracked = (hf & 2) != 0;
    trackview_get_pose(b, &h.pos, &h.rot);

    for (LobbyView::Controller &c : v->controllers)
        c.visible = false;
    for (int k = trackview_get_u8(b); k > 0 && b.ok; k--) {
        const int i = trackview_get_u8(b);
        LobbyView::Controller tmp;
        LobbyView::Controller &c = i < LOBBY_CONTROLLERS ? v->controllers[i] : tmp;
        memset(&c, 0, sizeof(c));
        const uint8_t f = trackview_get_u8(b);
        c.visible = true;
        c.tracked = (f & 1) != 0;
        c.pad_touch = (f & 2) != 0;
        c.pad_click = (f & 4) != 0;
        c.charging = (f & 8) != 0;
        c.hand_letter = (char)trackview_get_u8(b);
        trackview_get(b, &c.buttons, 2);
        trackview_get_pose(b, &c.pos, &c.rot);
        trackview_get(b, &c.rgb, 4);
        c.trigger = trackview_get_f32(b);
        c.battery = trackview_get_f32(b);
        c.pad_x = trackview_get_f32(b);
        c.pad_y = trackview_get_f32(b);
    }

    for (LobbyView::Pad &p : v->pads)
        p.visible = false;
    for (int k = trackview_get_u8(b); k > 0 && b.ok; k--) {
        const int i = trackview_get_u8(b);
        LobbyView::Pad tmp;
        LobbyView::Pad &p = i < LOBBY_PADS ? v->pads[i] : tmp;
        memset(&p, 0, sizeof(p));
        const uint8_t f = trackview_get_u8(b);
        p.visible = true;
        p.tracked = (f & 1) != 0;
        p.touch[0] = (f & 2) != 0;
        p.touch[1] = (f & 4) != 0;
        p.charging = (f & 8) != 0;
        p.floating = (f & 16) != 0;
        p.label = (char)trackview_get_u8(b);
        trackview_get(b, &p.buttons, 4);
        trackview_get_pose(b, &p.pos, &p.rot);
        trackview_get(b, &p.rgb, 4);
        float g[13];
        trackview_get(b, g, sizeof(g));
        p.lx = g[0], p.ly = g[1], p.rx = g[2], p.ry = g[3], p.l2 = g[4], p.r2 = g[5];
        p.touch_x[0] = g[6], p.touch_x[1] = g[7], p.touch_y[0] = g[8], p.touch_y[1] = g[9];
        p.battery = g[10], p.rumble_large = g[11], p.rumble_small = g[12];
    }
    *seq_out = seq;
    return b.ok;
}
