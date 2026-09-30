#include "hmd.h"

#include <string.h>

#include <orbis/libkernel.h>

#include "log.h"

typedef int (*PFN_sceHmdInitialize)(const HmdInitializeParam *param);
typedef int (*PFN_sceHmdGetDeviceInformation)(HmdDeviceInformation *info);
typedef int (*PFN_sceHmdOpen)(int32_t user_id, int32_t type, int32_t index, void *param);
typedef int (*PFN_sceHmdGetFieldOfView)(int32_t handle, HmdFieldOfView *fov);

static PFN_sceHmdInitialize p_initialize;
static PFN_sceHmdGetDeviceInformation p_get_device_information;
static PFN_sceHmdOpen p_open;
static PFN_sceHmdGetFieldOfView p_get_field_of_view;
static int (*p_get_device_information_by_handle)(int32_t handle, HmdDeviceInformation *info);
static int (*p_close)(int32_t handle);

static_assert(sizeof(HmdDeviceInformation) == 0x20, "HmdDeviceInformation layout");

const char *hmd_status_name(uint32_t status)
{
    switch (status) {
    case HMD_STATUS_READY: return "READY";
    case HMD_STATUS_NOT_READY: return "NOT_READY";
    case HMD_STATUS_NOT_DETECTED: return "NOT_DETECTED";
    case HMD_STATUS_NOT_READY_HMU_DISCONNECT: return "NOT_READY_HMU_DISCONNECT";
    default: return "UNKNOWN";
    }
}

static bool resolve(int module, const char *name, void **fn)
{
    int rc = sceKernelDlsym(module, name, fn);
    if (rc != 0 || !*fn) {
        LOG("hmd: symbol %s not found (0x%08x)", name, (unsigned)rc);
        return false;
    }
    return true;
}

int hmd_refresh(HmdState *st)
{
    memset(&st->info, 0, sizeof(st->info));
    int rc = p_get_device_information(&st->info);
    if (rc < 0)
        LOG("sceHmdGetDeviceInformation -> 0x%08x", (unsigned)rc);
    return rc;
}

bool hmd_init(int module, HmdState *st)
{
    memset(st, 0, sizeof(*st));
    if (module < 0)
        return false;
    if (!resolve(module, "sceHmdInitialize", (void **)&p_initialize) ||
        !resolve(module, "sceHmdGetDeviceInformation", (void **)&p_get_device_information) ||
        !resolve(module, "sceHmdGetDeviceInformationByHandle", (void **)&p_get_device_information_by_handle) ||
        !resolve(module, "sceHmdOpen", (void **)&p_open) || !resolve(module, "sceHmdClose", (void **)&p_close) ||
        !resolve(module, "sceHmdGetFieldOfView", (void **)&p_get_field_of_view))
        return false;

    HmdInitializeParam param;
    memset(&param, 0, sizeof(param));
    int rc = p_initialize(&param);
    LOG("sceHmdInitialize -> 0x%08x", (unsigned)rc);
    if (rc < 0 && (unsigned)rc != 0x81110001 /* already initialized */)
        return false;
    st->initialized = true;
    return true;
}

bool hmd_open(int user_id, HmdState *st)
{
    if (!st->initialized)
        return false;
    if (hmd_refresh(st) == 0) {
        LOG("HMD status=%s user=0x%x panel=%ux%u latency90=%u latency120=%u hmu_mount=%u",
            hmd_status_name(st->info.status), st->info.user_id, st->info.panel_width,
            st->info.panel_height, st->info.flip_to_display_latency_90hz,
            st->info.flip_to_display_latency_120hz, st->info.hmu_mount);
    }

    int rc = p_open(user_id, 0, 0, nullptr);
    LOG("sceHmdOpen(user=0x%x) -> 0x%08x", user_id, (unsigned)rc);
    if (rc < 0)
        return false;
    st->handle = rc;

    rc = p_get_field_of_view(st->handle, &st->fov);
    LOG("sceHmdGetFieldOfView -> 0x%08x  tan out=%.4f in=%.4f top=%.4f bottom=%.4f", (unsigned)rc,
        st->fov.tan_out, st->fov.tan_in, st->fov.tan_top, st->fov.tan_bottom);
    if (rc < 0 || st->fov.tan_out + st->fov.tan_in <= 0.0f || st->fov.tan_top + st->fov.tan_bottom <= 0.0f) {
        // The values every PSVR reported so far: a zero field of view would divide by zero
        // in the lobby and be sent to the streamer.
        st->fov.tan_out = 1.2074f;
        st->fov.tan_in = 1.1813f;
        st->fov.tan_top = st->fov.tan_bottom = 1.2629f;
        LOG("sceHmdGetFieldOfView failed: the usual PSVR field of view is used");
    }
    return true;
}

bool hmd_handle_valid(HmdState *st)
{
    if (st->handle <= 0)
        return false;
    HmdDeviceInformation info;
    memset(&info, 0, sizeof(info));
    int rc = p_get_device_information_by_handle(st->handle, &info);
    if (rc < 0)
        LOG("sceHmdGetDeviceInformationByHandle(0x%x) -> 0x%08x", st->handle, (unsigned)rc);
    return (unsigned)rc != 0x81110003; // HANDLE_INVALID: the headset was power cycled or replugged
}

bool hmd_reopen(int user_id, HmdState *st)
{
    if (st->handle > 0) {
        LOG("sceHmdClose(0x%x) -> 0x%08x", st->handle, (unsigned)p_close(st->handle));
        st->handle = 0;
    }
    HmdFieldOfView fov = st->fov;
    bool ok = hmd_open(user_id, st);
    if (!ok)
        st->fov = fov;
    return ok;
}

void hmd_stop(int module, HmdState *st)
{
    if (st->handle > 0) {
        auto close = (int (*)(int32_t))nullptr;
        if (resolve(module, "sceHmdClose", (void **)&close))
            LOG("sceHmdClose(0x%x) -> 0x%08x", st->handle, (unsigned)close(st->handle));
        st->handle = 0;
    }
    if (st->initialized) {
        auto terminate = (int (*)())nullptr;
        if (resolve(module, "sceHmdTerminate", (void **)&terminate))
            LOG("sceHmdTerminate -> 0x%08x", (unsigned)terminate());
        st->initialized = false;
    }
}
