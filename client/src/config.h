#pragma once

// Persistent client settings, kept in the PS4 save data of the user who started the app.

#define ALVR_STREAMER_VERSION "20.14.1"
// Headset prediction by default, in percent of the stream latency (100 overshot on hardware).
#define CONFIG_DEFAULT_HEAD_PREDICTION 40

struct ClientConfig {
    char hostname[64]; // ALVR client hostname, "NNNN.client" like the official client
    // Stream resolution per eye, in percent of the PSVR panel (960x1080). Above 100 the
    // PC renders more pixels than the panel has, which keeps text sharp through the lens
    // distortion (PSVR games do the same).
    // Past 2048 pixels of stream width (107%), decoding became several times slower.
    int resolution_percent;
    // Extra controller prediction on top of SteamVR's own (which uses the velocities).
    int controller_prediction_ms;
    // Headset position sent to SteamVR predicted over this share of the stream latency
    // (0-100 %; 0 = the current position, as up to 0.9.3).
    int head_prediction_percent;
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
    // Headset refresh rate, 90 or 60 (next launch). 90: the PSVR runs at 90 Hz and the PC
    // streams 90 frames per second; 60: the PSVR runs at 120 Hz, each frame shown twice.
    int refresh_rate;
    // PS Move (and DualShock 4) vibration strength, 0-100 % of the full strength (0 = off),
    // for the game's haptics and the lobby's alike.
    int vibration_percent;
    // Performance overlay in the headset while streaming (1) or not (0).
    int hud;
    // Settings panel opened each time the lobby comes back from the stream (1, default).
    int lobby_settings;
    // Trackpad behaviour per hand (index 0 left, 1 right; wand.h). pad_swap: 0 (default) =
    // the Move button touches and TRIANGLE (left) / SQUARE (right) clicks, 1 = the other way
    // round (the two never do the same). pad_alt_move / pad_alt_other: that button's point
    // is 0 = Default (drag from the pad centre by rotating the controller) or 1 = Alternate
    // (where the controller points, relative to the headset).
    int pad_swap[2], pad_alt_move[2], pad_alt_other[2];
};

// Reads the settings from the save data (defaults when there is none yet).
void config_load(ClientConfig *cfg, int user_id);
// Saves them in the background (after config_load).
void config_store(const ClientConfig *cfg);
