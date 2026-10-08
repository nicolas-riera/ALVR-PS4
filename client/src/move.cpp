#include "move.h"

#include <pthread.h>
#include <string.h>

#include <orbis/libkernel.h>

#include "hid.h"
#include "log.h"

// Structures follow shadPS4 (src/core/libraries/move/move.h).
struct MoveDeviceInfo {
    float sphere_radius;
    float accelerometer_offset[3];
};

struct MoveData {
    float accelerometer[3];
    float gyro[3];
    uint16_t buttons;
    uint16_t trigger;
    uint16_t ext_status, ext_digital0, ext_digital1, ext_analog[4];
    uint8_t ext_custom[5];
    int64_t timestamp;
    int32_t count;
    float temperature;
};
static_assert(sizeof(MoveData) == 0x40, "MoveData size");

typedef int (*PFN_Init)();
typedef int (*PFN_Open)(int32_t user_id, int32_t type, int32_t index);
typedef int (*PFN_GetDeviceInfo)(int32_t handle, MoveDeviceInfo *info);
typedef int (*PFN_ReadStateLatest)(int32_t handle, MoveData *data);
typedef int (*PFN_SetLightSphere)(int32_t handle, uint8_t r, uint8_t g, uint8_t b);
typedef int (*PFN_SetVibration)(int32_t handle, uint8_t intensity);

static PFN_ReadStateLatest p_read_latest;
static PFN_SetLightSphere p_set_sphere;
static PFN_GetDeviceInfo p_get_info;
static PFN_SetVibration p_set_vibration;

static void *resolve(int module, const char *name)
{
    void *fn = nullptr;
    if (sceKernelDlsym(module, name, &fn) != 0 || !fn) {
        LOG("move: symbol %s not found", name);
        return nullptr;
    }
    return fn;
}

// Raw HID input reports (hid.h): the Move handle is the HID handle; the PS Move input
// report has its id at data[1] and the battery at data[0x0d]. Reports are consumed when
// read, so polling steals one sample from libSceMove: done only every 5 s.
static int read_battery(int index, int handle)
{
    HidReport rep;
    int32_t device_id = 0;
    int n = hid_read_report(handle, &rep, &device_id);
    static bool logged[MOVE_MAX];
    if (!logged[index] && n > 0) {
        logged[index] = true;
        const uint8_t *d = rep.data;
        LOG("move %d: raw report (count %d, device %d): flags %02x id %02x buttons %02x%02x%02x%02x trigger %02x "
            "battery %02x temperature %u",
            index, n, device_id, d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[0x0d], d[0x26] << 4 | d[0x27] >> 4);
    }
    if (n <= 0 || device_id == 0)
        return -1;
    const int b = rep.data[0x0d];
    return b <= 5 || b == 0xEE || b == 0xEF ? b : -1;
}

bool move_battery(const MoveController &c, float *gauge, bool *charging)
{
    const int b = c.battery_raw;
    if (!c.connected || b < 0)
        return false;
    *charging = b == 0xEE || b == 0xEF;
    *gauge = b == 0xEF ? 1.0f : b == 0xEE ? 0.5f : b / 5.0f; // level unknown while charging
    return true;
}

uint32_t move_led_rgb(uint32_t led_color)
{
    // SceVrTrackerLedColor: blue, red, cyan (also listed as green), magenta, yellow.
    switch (led_color) {
    case 0: return 0x0000ff;
    case 1: return 0xff0000;
    case 2: return 0x00ffff;
    case 3: return 0xff00ff;
    case 4: return 0xffff00;
    default: return 0xffffff;
    }
}

void move_start(int module, int user_id, MoveController ctl[MOVE_MAX])
{
    for (int i = 0; i < MOVE_MAX; i++) {
        memset(&ctl[i], 0, sizeof(ctl[i]));
        ctl[i].handle = -1;
        ctl[i].battery_raw = -1;
    }
    if (module < 0)
        return;
    static PFN_Open open;
    static bool initialized;
    if (!initialized) {
        initialized = true;
        auto init = (PFN_Init)resolve(module, "sceMoveInit");
        open = (PFN_Open)resolve(module, "sceMoveOpen");
        p_get_info = (PFN_GetDeviceInfo)resolve(module, "sceMoveGetDeviceInfo");
        p_read_latest = (PFN_ReadStateLatest)resolve(module, "sceMoveReadStateLatest");
        p_set_sphere = (PFN_SetLightSphere)resolve(module, "sceMoveSetLightSphere");
        p_set_vibration = (PFN_SetVibration)resolve(module, "sceMoveSetVibration");
        if (!init || !open || !p_get_info || !p_read_latest || !p_set_sphere) {
            open = nullptr;
            return;
        }
        LOG("sceMoveInit -> 0x%08x", (unsigned)init());
    }
    if (!open)
        return;
    int rc;
    for (int i = 0; i < MOVE_MAX; i++) {
        ctl[i].owner = user_id;
        int h = open(user_id, 0, i);
        LOG("sceMoveOpen(user 0x%x, index %d) -> 0x%08x", user_id, i, (unsigned)h);
        if (h < 0)
            continue;
        ctl[i].handle = h;
        MoveDeviceInfo info;
        memset(&info, 0, sizeof(info));
        rc = p_get_info(h, &info);
        ctl[i].connected = rc == 0;
        ctl[i].sphere_radius = info.sphere_radius;
        LOG("move %d: GetDeviceInfo -> 0x%08x sphere_radius=%.4f", i, (unsigned)rc, info.sphere_radius);
        // Registered with the tracker once connected (see move_update): registering a
        // sleeping controller left its sphere dark until the tracker was restarted.
        ctl[i].track.handle = h;
    }
}

void move_update(MoveController ctl[MOVE_MAX])
{
    for (int i = 0; i < MOVE_MAX; i++) {
        MoveController &c = ctl[i];
        if (c.handle < 0)
            continue;
        uint64_t now = sceKernelGetProcessTime();
        // Battery first: every libSceMove state call drains the pending HID reports, so
        // right after ReadStateLatest the queue was almost always empty (one controller
        // never got a reading). A failed read takes no report: retry on the next frame.
        if (c.connected && now >= c.next_battery_us) {
            int b = read_battery(i, c.handle);
            if (b >= 0 && b != c.battery_raw)
                LOG("move %d: battery 0x%02x", i, b);
            if (b >= 0) {
                c.battery_raw = b;
                c.next_battery_us = now + 5000000;
            }
        }
        MoveData d;
        memset(&d, 0, sizeof(d));
        int rc = p_read_latest(c.handle, &d);
        bool connected = rc == 0;
        if (connected != c.connected) {
            LOG("move %d of user 0x%x: %s (ReadStateLatest 0x%08x)", i, c.owner, connected ? "connected" : "disconnected",
                (unsigned)rc);
            c.connected = connected;
            c.battery_raw = -1;
            c.next_battery_us = now;
        }
        if (connected && !c.track.registered && now >= c.next_register_us) {
            if (!tracker_register_device(&c.track, TRACKER_DEVICE_MOVE, c.handle, c.owner))
                c.next_register_us = now + 2000000; // retry in 2 s
        }
        else if (!connected && c.track.registered)
            tracker_unregister_device(&c.track);
        if (connected) {
            if (d.buttons != c.buttons)
                LOG("move %d: buttons 0x%04x -> 0x%04x", i, c.buttons, d.buttons);
            c.buttons = d.buttons;
            c.trigger = d.trigger;
            for (int k = 0; k < 3; k++) {
                c.accel[k] = d.accelerometer[k];
                c.gyro[k] = d.gyro[k];
            }
        }
        move_vibration_expire(&c);
        // The sphere is driven by the VR tracker itself (as it does for the headset
        // LEDs); setting it from here fought with the tracker and switched it off.
        tracker_update_device(&c.track);
    }
}

static int g_vibration_percent = 100;

void move_set_vibration_strength(int percent)
{
    g_vibration_percent = percent < 0 ? 0 : percent > 100 ? 100 : percent;
}

// Game haptics arrive on the network thread while the main thread starts and ends pulses:
// the motor state and its cached value change together under this lock (a motor could stay
// on when the cache said off).
static pthread_mutex_t g_vibration_lock = PTHREAD_MUTEX_INITIALIZER;

static void vibrate_locked(MoveController *c, uint8_t intensity, uint32_t duration_ms)
{
    if (c->handle < 0 || !p_set_vibration)
        return;
    intensity = (uint8_t)(intensity * g_vibration_percent / 100); // strength setting (0 = off)
    const uint64_t now = sceKernelGetProcessTime();
    c->vibration_end_us = duration_ms ? now + duration_ms * 1000ull : 0;
    if (intensity == c->vibration)
        return;
    int rc = p_set_vibration(c->handle, intensity);
    if (rc != 0)
        LOG("sceMoveSetVibration(0x%x, %u) -> 0x%08x", c->handle, intensity, (unsigned)rc);
    c->vibration = intensity;
    c->vibration_sent_us = now;
}

void move_vibrate(MoveController *c, uint8_t intensity, uint32_t duration_ms)
{
    pthread_mutex_lock(&g_vibration_lock);
    vibrate_locked(c, intensity, duration_ms);
    pthread_mutex_unlock(&g_vibration_lock);
}

void move_vibration_expire(MoveController *c)
{
    pthread_mutex_lock(&g_vibration_lock);
    const uint64_t now = sceKernelGetProcessTime();
    if (c->vibration_end_us && now >= c->vibration_end_us) {
        vibrate_locked(c, 0, 0);
    } else if (c->vibration && c->handle >= 0 && p_set_vibration && now - c->vibration_sent_us >= 1000000) {
        // The controller stops its motor by itself a few seconds after the last command
        // (hardware report: long vibrations stopped): a long one is sent again every second.
        p_set_vibration(c->handle, c->vibration);
        c->vibration_sent_us = now;
    }
    pthread_mutex_unlock(&g_vibration_lock);
}
