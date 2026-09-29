#pragma once

// Persistent client settings, stored in /data/alvr-ps4/config.txt.

#define ALVR_STREAMER_VERSION "20.14.1"

struct ClientConfig {
    char hostname[64]; // ALVR client hostname, "NNNN.client" like the official client
    // Stream resolution per eye, in percent of the PSVR panel (960x1080). Above 100 the
    // PC renders more pixels than the panel has, which keeps text sharp through the lens
    // distortion (PSVR games do the same).
    // Past 2048 pixels of stream width (107%), decoding became several times slower.
    int resolution_percent;
    // Extra controller prediction on top of SteamVR's own (which uses the velocities).
    int controller_prediction_ms;
    // Height of the PS Camera above the floor, derived from the user's height in the lobby
    // settings. 0 = not set yet: the floor is guessed from the first headset position.
    int camera_height_cm;
    // User's height, set in the lobby settings (0 = not set). Each change places the floor
    // below the headset at the matching eye height, stored as camera_height_cm.
    int user_height_cm;
    // Play space centre placed on the headset each time SteamVR connects (1, default) or
    // only once at the first tracking (0: for the tracker mode, where Space Calibrator's
    // calibration must survive SteamVR restarts).
    int center_on_connect;
};

void config_load(ClientConfig *cfg);
void config_store(const ClientConfig *cfg);
