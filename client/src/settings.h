#pragma once

#include <stdint.h>

#include "config.h"
#include "lobby.h"
#include "vrmath.h"

// Lobby settings panel: opened with START, placed in front of the head (towards the
// camera, whatever the head orientation), driven by a laser from each PS Move and
// clicked with the trigger. Changes go to /data/alvr-ps4/config.txt.

// Height shown until the user sets theirs; the floor is estimated at its eye height
// below the headset until then, so the grid matches the value shown.
static const int SETTINGS_DEFAULT_USER_HEIGHT_CM = 170;
static const float SETTINGS_EYE_HEIGHT_RATIO = 0.936f; // eye height / body height (adult average)

struct SettingsContext {
    ClientConfig *config;
    float floor_y;   // tracker-space floor height in use
    float head_y;    // tracker-space height of the centre between the eyes
    int moves_connected;
};

struct SettingsRay {
    bool valid;
    Vec3 origin, dir; // tracker space, dir normalized
    float trigger;    // 0..1
};

enum SettingsAction : unsigned {
    SETTINGS_HEIGHT_CHANGED = 2, // user height changed: config->camera_height_cm (floor) updated
    SETTINGS_RESET = 4,          // every setting back to its default, panel closed (the wizard comes back)
    SETTINGS_CONFIRMED = 8,      // the first launch wizard was confirmed (height set, panel closed)
};

void settings_open(Vec3 head_pos);
// First launch wizard: only the height and a Confirm button; settings_close() leaves it
// open, it closes with Confirm (SETTINGS_CONFIRMED).
void settings_open_wizard(Vec3 head_pos);
void settings_close();
bool settings_is_open();
bool settings_is_wizard();
// Once per lobby frame while open. Returns SettingsAction bits; *clicked_hand is set to
// the Move index whose trigger clicked a button (-1 if none), for a haptic tick.
unsigned settings_update(const SettingsContext &ctx, const SettingsRay rays[2], uint64_t now_us, int *clicked_hand);
// Fills the panel and the two lasers for the lobby renderer.
void settings_build(LobbyPanel *panel, LobbyPointer pointers[2]);
