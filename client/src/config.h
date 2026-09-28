#pragma once

// Persistent client settings, stored in /data/alvr-ps4/config.txt.

#define ALVR_STREAMER_VERSION "20.14.1"

struct ClientConfig {
    char hostname[64]; // ALVR client hostname, "NNNN.client" like the official client
    // Stream resolution per eye, in percent of the PSVR panel (960x1080). Above 100 the
    // PC renders more pixels than the panel has, which keeps text sharp through the lens
    // distortion (PSVR games do the same).
    int resolution_percent;
    // Extra controller prediction on top of SteamVR's own (which uses the velocities).
    int controller_prediction_ms;
};

void config_load(ClientConfig *cfg);
void config_store(const ClientConfig *cfg);
