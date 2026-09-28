#pragma once

#include <stdint.h>

// ALVR 20.14.1 client (headset side): discovery, control handshake, keepalive,
// tracking/input uplink and haptics downlink. Wire format: docs/alvr-20.14.1-protocol.md.

enum AlvrState {
    ALVR_DISCOVERY,  // announcing, waiting for the streamer to connect
    ALVR_HANDSHAKE,  // control socket open, negotiating
    ALVR_RESTARTING, // streamer asked to restart SteamVR; will reconnect
    ALVR_STREAMING,  // stream started
};

struct AlvrStatus {
    AlvrState state;
    char server_ip[16];
    uint32_t view_width, view_height; // negotiated per-eye stream size
    float refresh_rate;
    unsigned video_packets;           // video packets received (decoding comes later)
};

struct AlvrDeviceMotion {
    bool present;          // false: omitted from the packet (controller lost/off)
    float orientation[4];  // x, y, z, w (stage space)
    float position[3];     // metres, stage space (+Y up, -Z forward, floor origin)
    float linear_velocity[3];
    float angular_velocity[3];
};

struct AlvrViews {
    float ipd_m;
    float fov[2][4]; // per eye: left, right, up, down angles in radians (OpenXR signs)
};

struct AlvrHandInput {
    bool trackpad_touch, trackpad_click;
    float trackpad_x, trackpad_y;
    bool grip, menu, system;
    float trigger;
    bool trigger_click;
};

typedef void (*AlvrHapticsCallback)(int hand /*0 left, 1 right*/, float duration_s, float frequency,
                                    float amplitude);

void alvr_start(const char *hostname, const char *local_ip, const AlvrViews *views, AlvrHapticsCallback haptics);
void alvr_get_status(AlvrStatus *out);

// Uplink, called from the render loop. timestamp_ns must be strictly increasing.
void alvr_send_tracking(uint64_t timestamp_ns, const AlvrDeviceMotion *head, const AlvrDeviceMotion *left,
                        const AlvrDeviceMotion *right);
void alvr_update_input(int hand, const AlvrHandInput *in);
