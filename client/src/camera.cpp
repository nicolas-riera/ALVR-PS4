#include "camera.h"

#include <string.h>

#include <orbis/libkernel.h>

#include "log.h"

typedef int (*PFN_Open)(int32_t user_id, int32_t type, int32_t index, void *param);
typedef int (*PFN_SetConfig)(int32_t handle, void *config);
typedef int (*PFN_SetVideoSync)(int32_t handle, void *param);
typedef int (*PFN_Start)(int32_t handle, void *param);
typedef int (*PFN_GetFrameData)(int32_t handle, void *frame_data);

static PFN_GetFrameData p_get_frame_data;

struct VideoSyncParam {
    uint32_t size; // 0x10
    uint32_t mode;
    void *option;
};

struct StartParam {
    uint32_t size; // 0x18
    uint32_t format_level[2];
    void *option;
};

static_assert(sizeof(VideoSyncParam) == 0x10, "VideoSyncParam size");
static_assert(sizeof(StartParam) == 0x18, "StartParam size");

static void *resolve(int module, const char *name)
{
    void *fn = nullptr;
    if (sceKernelDlsym(module, name, &fn) != 0 || !fn) {
        LOG("camera: symbol %s not found", name);
        return nullptr;
    }
    return fn;
}

bool camera_start(int module, CameraState *st)
{
    st->handle = -1;
    if (module < 0)
        return false;
    auto open = (PFN_Open)resolve(module, "sceCameraOpen");
    auto set_config = (PFN_SetConfig)resolve(module, "sceCameraSetConfig");
    auto set_video_sync = (PFN_SetVideoSync)resolve(module, "sceCameraSetVideoSync");
    auto start = (PFN_Start)resolve(module, "sceCameraStart");
    p_get_frame_data = (PFN_GetFrameData)resolve(module, "sceCameraGetFrameData");
    if (!open || !set_config || !set_video_sync || !start || !p_get_frame_data)
        return false;

    // User 0xff (system), as the games do.
    int handle = open(0xff, 0, 0, nullptr);
    LOG("sceCameraOpen -> 0x%08x", (unsigned)handle);
    if (handle < 0)
        return false;

    // Games use config type 5 with VR tracker profile 100 and fall back to type 4
    // with profile 0 (reference/decomp/beatsaber_psvr.c); type 5 is refused by some
    // camera/firmware combinations. The extension part stays zeroed.
    int rc = -1;
    // Type 5 + profile 100 is accepted with SDK 5.50 but the tracker then never leaves
    // NOT_STARTED; type 4 + profile 0 tracks fine. Use type 4 only.
    static const uint32_t types[] = {4};
    for (uint32_t type : types) {
        uint8_t config[0x68];
        memset(config, 0, sizeof(config));
        *(uint32_t *)(config + 0) = sizeof(config);
        *(uint32_t *)(config + 4) = type;
        rc = set_config(handle, config);
        LOG("sceCameraSetConfig(type %u) -> 0x%08x", type, (unsigned)rc);
        if (rc >= 0) {
            st->config_type = type;
            break;
        }
    }
    if (rc < 0)
        return false;

    VideoSyncParam vs = {sizeof(VideoSyncParam), 1, nullptr};
    rc = set_video_sync(handle, &vs);
    LOG("sceCameraSetVideoSync(1) -> 0x%08x", (unsigned)rc);

    StartParam sp = {sizeof(StartParam), {0xf, 0xf}, nullptr};
    rc = start(handle, &sp);
    LOG("sceCameraStart -> 0x%08x", (unsigned)rc);
    if (rc < 0)
        return false;

    st->handle = handle;
    return true;
}

int camera_get_frame(CameraState *st, void *frame_data)
{
    memset(frame_data, 0, CAMERA_FRAME_DATA_SIZE);
    *(uint32_t *)frame_data = CAMERA_FRAME_DATA_SIZE;
    *((uint32_t *)frame_data + 1) = 0x11; // read mode used by the games
    return p_get_frame_data(st->handle, frame_data);
}

void camera_stop(int module, CameraState *st)
{
    if (st->handle < 0)
        return;
    auto stop = (int (*)(int32_t))resolve(module, "sceCameraStop");
    auto close = (int (*)(int32_t))resolve(module, "sceCameraClose");
    if (stop)
        LOG("sceCameraStop -> 0x%08x", (unsigned)stop(st->handle));
    if (close)
        LOG("sceCameraClose -> 0x%08x", (unsigned)close(st->handle));
    st->handle = -1;
}
