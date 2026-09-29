#include "move.h"

#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>

#include <orbis/libkernel.h>

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

// Raw HID input reports, as libSceMove reads them (reference/decomp/move_battery.c): the
// Move handle is the HID handle; each entry is a timestamp, a kernel flag byte, then the PS
// Move input report (report id at data[1], battery at data[0x0d]). Reports are consumed
// when read, so polling steals one sample from libSceMove: done only every 5 s.
struct HidReport {
    uint64_t timestamp_us;
    uint8_t data[0x38];
};

struct HidReadReports {
    uint32_t handle;
    uint32_t pad0;
    HidReport *reports;
    uint32_t max_reports;
    uint32_t pad1;
    int32_t *device_id;
};
static_assert(sizeof(HidReadReports) == 0x20, "ioctl 0xc0204834 argument size");

static int read_battery(int index, int handle)
{
    static int fd = -2;
    if (fd == -2) {
        fd = open("/dev/hid", O_RDONLY);
        LOG("move: open(/dev/hid) -> %d", fd);
    }
    if (fd < 0)
        return -1;
    HidReport rep;
    int32_t device_id = 0;
    HidReadReports req;
    memset(&req, 0, sizeof(req));
    memset(&rep, 0, sizeof(rep));
    req.handle = (uint32_t)handle;
    req.reports = &rep;
    req.max_reports = 1;
    req.device_id = &device_id;
    int n = ioctl(fd, 0xc0204834, &req);
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
    auto init = (PFN_Init)resolve(module, "sceMoveInit");
    auto open = (PFN_Open)resolve(module, "sceMoveOpen");
    p_get_info = (PFN_GetDeviceInfo)resolve(module, "sceMoveGetDeviceInfo");
    p_read_latest = (PFN_ReadStateLatest)resolve(module, "sceMoveReadStateLatest");
    p_set_sphere = (PFN_SetLightSphere)resolve(module, "sceMoveSetLightSphere");
    p_set_vibration = (PFN_SetVibration)resolve(module, "sceMoveSetVibration");
    if (!init || !open || !p_get_info || !p_read_latest || !p_set_sphere)
        return;
    int rc = init();
    LOG("sceMoveInit -> 0x%08x", (unsigned)rc);

    for (int i = 0; i < MOVE_MAX; i++) {
        int h = open(user_id, 0, i);
        LOG("sceMoveOpen(index %d) -> 0x%08x", i, (unsigned)h);
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
            LOG("move %d: %s (ReadStateLatest 0x%08x)", i, connected ? "connected" : "disconnected", (unsigned)rc);
            c.connected = connected;
            c.battery_raw = -1;
            c.next_battery_us = now;
        }
        if (connected && !c.track.registered && now >= c.next_register_us) {
            if (!tracker_register_device(&c.track, TRACKER_DEVICE_MOVE, c.handle))
                c.next_register_us = now + 2000000; // retry in 2 s
        }
        else if (!connected && c.track.registered)
            tracker_unregister_device(&c.track);
        if (connected) {
            if (d.buttons != c.buttons)
                LOG("move %d: buttons 0x%04x -> 0x%04x", i, c.buttons, d.buttons);
            c.buttons = d.buttons;
            c.trigger = d.trigger;
        }
        if (c.vibration_end_us && sceKernelGetProcessTime() >= c.vibration_end_us)
            move_vibrate(&c, 0, 0);
        // The sphere is driven by the VR tracker itself (as it does for the headset
        // LEDs); setting it from here fought with the tracker and switched it off.
        tracker_update_device(&c.track);
    }
}

void move_vibrate(MoveController *c, uint8_t intensity, uint32_t duration_ms)
{
    if (c->handle < 0 || !p_set_vibration)
        return;
    c->vibration_end_us = duration_ms ? sceKernelGetProcessTime() + duration_ms * 1000ull : 0;
    if (intensity == c->vibration)
        return;
    int rc = p_set_vibration(c->handle, intensity);
    if (rc != 0)
        LOG("sceMoveSetVibration(0x%x, %u) -> 0x%08x", c->handle, intensity, (unsigned)rc);
    c->vibration = intensity;
}
