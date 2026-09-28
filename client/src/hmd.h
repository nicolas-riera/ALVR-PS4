#pragma once

#include <stdint.h>

// libSceHmd bindings. Structures and error codes follow shadPS4
// (src/core/libraries/hmd); functions are resolved at runtime with
// sceKernelDlsym on the handle of the already-loaded module.

enum HmdDeviceStatus : uint32_t {
    HMD_STATUS_READY = 0,
    HMD_STATUS_NOT_READY = 1,
    HMD_STATUS_NOT_DETECTED = 2,
    HMD_STATUS_NOT_READY_HMU_DISCONNECT = 3,
};

struct HmdInitializeParam {
    void *reserved0;
    uint8_t reserved[8];
};

struct HmdFieldOfView {
    float tan_out;
    float tan_in;
    float tan_top;
    float tan_bottom;
};

struct HmdDeviceInformation {
    HmdDeviceStatus status;
    int32_t user_id;
    uint8_t reserve0[4];
    uint32_t panel_width;
    uint32_t panel_height;
    uint16_t flip_to_display_latency_90hz;
    uint16_t flip_to_display_latency_120hz;
    uint8_t hmu_mount;
    uint8_t reserve1[7];
};

struct HmdState {
    bool initialized;
    int handle; // >0 once sceHmdOpen succeeded
    HmdDeviceInformation info;
    HmdFieldOfView fov;
};

const char *hmd_status_name(uint32_t status);

// Initializes libSceHmd and opens the headset for the given user.
// Leaving the headset open is what makes the system treat us as a VR app.
bool hmd_start(int module_handle, int user_id, HmdState *out);
// Re-reads the device information (status changes when the PSVR is powered).
int hmd_refresh(HmdState *st);
// Closes the headset and terminates libSceHmd, so the system gets the PSVR back.
void hmd_stop(int module_handle, HmdState *st);
