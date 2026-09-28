#include "move.h"

#include <string.h>

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

static PFN_ReadStateLatest p_read_latest;
static PFN_SetLightSphere p_set_sphere;
static PFN_GetDeviceInfo p_get_info;

static void *resolve(int module, const char *name)
{
    void *fn = nullptr;
    if (sceKernelDlsym(module, name, &fn) != 0 || !fn) {
        LOG("move: symbol %s not found", name);
        return nullptr;
    }
    return fn;
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
    }
    if (module < 0)
        return;
    auto init = (PFN_Init)resolve(module, "sceMoveInit");
    auto open = (PFN_Open)resolve(module, "sceMoveOpen");
    p_get_info = (PFN_GetDeviceInfo)resolve(module, "sceMoveGetDeviceInfo");
    p_read_latest = (PFN_ReadStateLatest)resolve(module, "sceMoveReadStateLatest");
    p_set_sphere = (PFN_SetLightSphere)resolve(module, "sceMoveSetLightSphere");
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
        MoveData d;
        memset(&d, 0, sizeof(d));
        int rc = p_read_latest(c.handle, &d);
        bool connected = rc == 0;
        if (connected != c.connected) {
            LOG("move %d: %s (ReadStateLatest 0x%08x)", i, connected ? "connected" : "disconnected", (unsigned)rc);
            c.connected = connected;
        }
        uint64_t now = sceKernelGetProcessTime();
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
        // The sphere is driven by the VR tracker itself (as it does for the headset
        // LEDs); setting it from here fought with the tracker and switched it off.
        tracker_update_device(&c.track);
    }
}
