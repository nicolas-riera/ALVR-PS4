#include "pad.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>

#include <orbis/Pad.h>
#include <orbis/libkernel.h>

#include "log.h"

static float stick_axis(uint8_t v)
{
    float f = ((int)v - 128) / 127.0f;
    return f < -1.0f ? -1.0f : f > 1.0f ? 1.0f : f;
}

// Battery is not in libScePad's data. The pad handle is a /dev/hid handle (libScePad
// opens it like libSceMove, reference: dumps/system/libScePad.sprx @0x580), but the
// DualShock 4 is read with another ioctl than the Move: 0x8030482e returns the kernel's
// decoded state, 0xa8 bytes per report (libScePad's read @0x2198: +0x00 timestamp,
// +0x0c buttons, +0x10 sticks, +0x14 L2/R2, then motion and touch data). Where the
// battery sits in it is not known yet: the entry is logged (connect, then every 30 s) to
// find the byte that follows the level and the cable. A read steals the pending reports
// from libScePad, so it runs every 5 s.
struct PadStateEntry {
    uint8_t b[0xa8];
};
struct PadReadStates {
    uint32_t handle;
    uint32_t zero;
    PadStateEntry *entries;
    uint32_t max_entries;
    uint32_t pad0;
    int32_t *device_id;
    int32_t *status;
    uint64_t mode; // 2, as libScePad's read
};
static_assert(sizeof(PadReadStates) == 0x30, "ioctl 0x8030482e argument size");

static int read_battery(int handle)
{
    static int fd = -2;
    if (fd == -2) {
        fd = open("/dev/hid", O_RDONLY);
        LOG("pad: open(/dev/hid) -> %d", fd);
    }
    if (fd < 0)
        return -1;
    static PadStateEntry entries[4];
    int32_t device_id = 0, status = 0;
    PadReadStates req;
    memset(&req, 0, sizeof(req));
    memset(entries, 0, sizeof(entries));
    req.handle = (uint32_t)handle;
    req.entries = entries;
    req.max_entries = 4;
    req.device_id = &device_id;
    req.status = &status;
    req.mode = 2;
    const int n = ioctl(fd, 0x8030482e, &req);
    static int reads;
    const bool dump = n > 0 && reads++ % 6 == 0;
    if (dump || (n <= 0 && reads < 3)) {
        const uint8_t *e = entries[n > 4 ? 3 : n > 0 ? n - 1 : 0].b; // max_entries 4
        char hex[3][2 * 56 + 1];
        for (int line = 0; line < 3; line++)
            for (int i = 0; i < 56; i++)
                snprintf(hex[line] + 2 * i, 3, "%02x", e[line * 56 + i]);
        LOG("pad: state read -> %d (device %d, status %d)", n, device_id, status);
        for (int line = 0; line < 3; line++)
            LOG("pad: state +%02x %s", line * 56, hex[line]);
    }
    return -1; // byte not identified yet
}

bool pad_battery(const PadController &p, float *gauge, bool *charging)
{
    const int b = p.battery_raw;
    if (!p.connected || b < 0)
        return false;
    const int level = b & 0x0f;
    const bool cable = (b & 0x10) != 0;
    *charging = cable && level < 11;
    *gauge = level >= 10 ? 1.0f : level / 10.0f;
    return true;
}

void pad_start(int user_id, PadController *p)
{
    memset(p, 0, sizeof(*p));
    p->handle = -1;
    p->owner = user_id;
    p->battery_raw = -1;
    p->touch_res_x = 1920;
    p->touch_res_y = 943;
    static bool initialized;
    if (!initialized) {
        initialized = true;
        LOG("scePadInit -> 0x%08x", (unsigned)scePadInit());
    }
    int h = scePadOpen(user_id, 0, 0, nullptr);
    LOG("scePadOpen(user 0x%x) -> 0x%08x", user_id, (unsigned)h);
    int rc;
    if (h < 0)
        return;
    p->handle = h;
    OrbisPadInformation info;
    memset(&info, 0, sizeof(info));
    rc = scePadGetControllerInformation(h, &info);
    if (rc == 0 && info.touchResolutionX && info.touchResolutionY) {
        p->touch_res_x = info.touchResolutionX;
        p->touch_res_y = info.touchResolutionY;
    }
    LOG("pad: controller information -> 0x%08x (touchpad %ux%u, connection type %u, connected %d, class %d)",
        (unsigned)rc, info.touchResolutionX, info.touchResolutionY, info.connectionType, info.connected,
        info.deviceClass);
    // Registered with the tracker once connected (pad_update), as the Moves are.
    p->track.handle = h;
}

void pad_update(PadController *p)
{
    if (p->handle < 0)
        return;
    const uint64_t now = sceKernelGetProcessTime();
    // Battery first, as for the Moves: the state call may drain the pending reports.
    if (ALVR_PS4_DEV && p->connected && now >= p->next_battery_us) { // probe: Dev build only
        int b = read_battery(p->handle);
        if (b >= 0 && b != p->battery_raw)
            LOG("pad: battery 0x%02x", b);
        if (b >= 0)
            p->battery_raw = b;
        p->next_battery_us = now + 5000000;
    }
    OrbisPadData d;
    memset(&d, 0, sizeof(d));
    int rc = scePadReadState(p->handle, &d);
    const bool connected = rc == 0 && d.connected;
    if (connected != p->connected) {
        LOG("pad of user 0x%x: %s (ReadState 0x%08x)", p->owner, connected ? "connected" : "disconnected",
            (unsigned)rc);
        p->connected = connected;
        p->battery_raw = -1;
        p->next_battery_us = now;
        if (connected) {
            const uint8_t *u = d.unknown; // connectedCount, reserve[2], deviceUniqueDataLen, deviceUniqueData[12]
            LOG("pad: count %u unique data (%u): %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
                d.count, u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9], u[10], u[11], u[12], u[13], u[14]);
        }
    }
    // Tracked only while its user has room: two PS Moves come first (tracker.h).
    if (connected && !p->track.registered && now >= p->next_register_us) {
        if (!tracker_register_device(&p->track, TRACKER_DEVICE_DUALSHOCK4, p->handle, p->owner))
            p->next_register_us = now + 2000000; // retry in 2 s
    } else if (!connected && p->track.registered) {
        tracker_unregister_device(&p->track);
    }
    // Set aside for a PS Move: its light bar gets the system colour back.
    if (p->was_tracked && !p->track.registered && connected)
        LOG("pad of user 0x%x: not tracked any more, light bar reset -> 0x%08x", p->owner,
            (unsigned)scePadResetLightBar(p->handle));
    p->was_tracked = p->track.registered;
    if (connected) {
        const uint32_t buttons = d.buttons & 0x10ffff;
        if (buttons != p->buttons)
            LOG("pad: buttons 0x%06x -> 0x%06x", p->buttons, buttons);
        p->buttons = buttons;
        p->lx = stick_axis(d.leftStick.x);
        p->ly = -stick_axis(d.leftStick.y);
        p->rx = stick_axis(d.rightStick.x);
        p->ry = -stick_axis(d.rightStick.y);
        p->l2 = d.analogButtons.l2 / 255.0f;
        p->r2 = d.analogButtons.r2 / 255.0f;
        for (int i = 0; i < 2; i++) {
            PadTouch &t = p->touch[i];
            t.down = i < d.touch.fingers;
            if (t.down) {
                t.x = d.touch.touch[i].x / (float)p->touch_res_x;
                t.y = d.touch.touch[i].y / (float)p->touch_res_y;
            }
        }
    } else {
        p->buttons = 0;
        p->lx = p->ly = p->rx = p->ry = p->l2 = p->r2 = 0.0f;
        p->touch[0].down = p->touch[1].down = false;
    }
    if (p->vibration_end_us && now >= p->vibration_end_us)
        pad_vibrate(p, 0, 0, 0);
    // The light bar is driven by the VR tracker (as the Move spheres are).
    tracker_update_device(&p->track);
}

static int g_vibration_percent = 100;

void pad_set_vibration_strength(int percent)
{
    g_vibration_percent = percent < 0 ? 0 : percent > 100 ? 100 : percent;
}

void pad_vibrate(PadController *p, uint8_t large, uint8_t small, uint32_t duration_ms)
{
    if (p->handle < 0)
        return;
    large = (uint8_t)(large * g_vibration_percent / 100); // strength setting (0 = off)
    small = (uint8_t)(small * g_vibration_percent / 100);
    p->vibration_end_us = duration_ms ? sceKernelGetProcessTime() + duration_ms * 1000ull : 0;
    if (large == p->vib_large && small == p->vib_small)
        return;
    OrbisPadVibeParam v{large, small};
    int rc = scePadSetVibration(p->handle, &v);
    if (rc != 0)
        LOG("scePadSetVibration(0x%x, %u, %u) -> 0x%08x", p->handle, large, small, (unsigned)rc);
    p->vib_large = large;
    p->vib_small = small;
}
