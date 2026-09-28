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
    TrackerPose eye_pose[2]; // left, right (from the HMD result, includes the system IPD)
    TrackerPose head_pose;
    uint64_t timestamp;
    unsigned results_ok;
};

// A tracked controller (PS Move). Pose handling follows Beat Saber: position and
// orientation are only updated while their quality is not NONE, otherwise the last
// value is held (the tracker itself dead-reckons NOT_VISIBLE phases from the IMU).
enum TrackerDeviceType : uint32_t {
    TRACKER_DEVICE_HMD = 0,
    TRACKER_DEVICE_DUALSHOCK4 = 1,
    TRACKER_DEVICE_MOVE = 2,
};

struct TrackedDevice {
    int handle;
    uint32_t type;
    bool registered;
    int last_rc;
    uint32_t status, position_quality, orientation_quality, led_color;
    bool has_position, has_orientation; // at least one valid sample so far
    float position[3];
    float orientation[4]; // x, y, z, w
    uint64_t timestamp;
};

const char *tracker_status_name(uint32_t status);
const char *tracker_quality_name(uint32_t q);

// Allocates the tracker memory, initializes libSceVrTracker and registers the headset.
bool tracker_start(int tracker_module, int camera_module, int hmd_handle, TrackerState *st);
// Starts the thread feeding camera frames (or motion sensor data) to the tracker.
void tracker_run_thread();
// Stops the thread, unregisters the HMD, terminates the tracker and closes the camera.
void tracker_stop(int tracker_module, int camera_module);
// Registers a controller with the tracker (after tracker_start).
bool tracker_register_device(TrackedDevice *d, uint32_t type, int handle);
void tracker_unregister_device(TrackedDevice *d);
// Reads the controller's latest result (call once per rendered frame).
void tracker_update_device(TrackedDevice *d);
// Called once per rendered frame: reads the HMD pose.
void tracker_update(TrackerState *st);
