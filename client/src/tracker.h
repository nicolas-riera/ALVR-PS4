#pragma once

#include <stdint.h>

// libSceVrTracker bindings (structures follow shadPS4 src/core/libraries/vr_tracker).

struct TrackerPose {
    float px, py, pz;
    uint32_t reserved0;
    float qx, qy, qz, qw;
    uint32_t reserved1[8];
};

struct TrackerState {
    bool initialized;
    bool hmd_registered;
    int camera_attached;
    int last_result_rc;
    int last_motion_rc;
    // Last successful HMD result
    uint32_t status;
    uint32_t position_quality;
    uint32_t orientation_quality;
    uint32_t led_color;
    TrackerPose device_pose;
    uint64_t timestamp;
    unsigned results_ok;
};

const char *tracker_status_name(uint32_t status);
const char *tracker_quality_name(uint32_t q);

// Allocates the tracker memory, initializes libSceVrTracker and registers the headset.
bool tracker_start(int tracker_module, int camera_module, int hmd_handle, TrackerState *st);
// Starts the thread feeding camera frames (or motion sensor data) to the tracker.
void tracker_run_thread();
// Stops the thread, unregisters the HMD, terminates the tracker and closes the camera.
void tracker_stop(int tracker_module, int camera_module);
// Called once per rendered frame: reads the HMD pose.
void tracker_update(TrackerState *st);
