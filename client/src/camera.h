#pragma once

#include <stdint.h>

// PlayStation Camera, configured the way PSVR games do it for libSceVrTracker
// (sequence and values taken from Beat Saber's PSVR subsystem, see
// reference/decomp/beatsaber_psvr.c).

#define CAMERA_FRAME_DATA_SIZE 0x248

struct CameraState {
    int handle; // >= 0 once opened and started
    uint32_t config_type; // 5 (tracker profile 100) or 4 (profile 0)
};

bool camera_start(int camera_module, CameraState *st);
// Fills a CAMERA_FRAME_DATA_SIZE-byte frame descriptor. Returns the SDK error code.
int camera_get_frame(CameraState *st, void *frame_data);
void camera_stop(int camera_module, CameraState *st);
