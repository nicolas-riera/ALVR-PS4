#pragma once

#include <stdint.h>

#include "config.h"
#include "lobby.h"
#include "vrmath.h"

// Lobby settings panel: opened with START (PS Move) or OPTIONS (DualShock 4), placed in
// front of the head (towards the camera, whatever the head orientation), driven by a
// laser from each PS Move (clicked with the trigger) and from the DualShock 4 (clicked
// with Cross). Changes go to the save data (config.cpp).
// "Calibrate" (settings and first launch wizard) opens the height calibration in its
// place: touch the floor with a PS Move's ball and pull its trigger, which measures the
// floor, or spread the arms holding both PS Moves and pull both triggers, which measures
// the height from the arm span. It then goes back to the panel it came from, with the
// height found.
// The height shown is the one the headset is at now above the floor in use (it follows
// the head); - / + move the floor under the headset to match the height set.

// Height shown until the user sets theirs.
static const int SETTINGS_DEFAULT_USER_HEIGHT_CM = 170;
// Headset (tracked eye centre) height / body height. 0.936 is the adult average of the eye
// height; the tracker's headset point sits lower: hardware test (1.73 m user, floor right
// when calibrated by the arm span) 0.905-0.910.
static const float SETTINGS_EYE_HEIGHT_RATIO = 0.908f;

struct SettingsContext {
    ClientConfig *config;
    float floor_y;   // tracker-space floor height in use
    float head_y;    // tracker-space height of the centre between the eyes
    int pointers_connected; // PS Moves and DualShock 4 that can click
    Vec3 head;       // tracker-space centre between the eyes
};

// Rays: Move 0, Move 1, DualShock 4 (SETTINGS_RAY_PAD).
static const int SETTINGS_RAY_PAD = LOBBY_POINTERS - 1;
struct SettingsRay {
    bool valid;
    Vec3 origin, dir; // tracker space, dir normalized; origin = the tracked point (sphere centre, light bar)
    float trigger;    // 0..1 (PS Move trigger; DualShock 4 Cross: 0 or 1)
    bool seen;        // position currently seen by the camera (height calibration)
    float scroll;     // DualShock 4 right stick, -1..1 (+1 up): scrolls the settings
};

enum SettingsAction : unsigned {
    SETTINGS_HEIGHT_CHANGED = 2, // user height changed: config->camera_height_cm (floor) updated
    SETTINGS_RESET = 4,          // every setting back to its default, panel closed (the wizard comes back)
    SETTINGS_CONFIRMED = 8,      // the first launch wizard was confirmed (height set, panel closed)
    SETTINGS_CALIBRATED = 16,    // the height calibration succeeded (with SETTINGS_HEIGHT_CHANGED)
    SETTINGS_VIBRATION_SET = 32, // vibration strength slider released: *clicked is the pointer, for a sample buzz
};

void settings_open(Vec3 head_pos);
// First launch wizard: only the height and a Confirm button; settings_close() leaves it
// open, it closes with Confirm (SETTINGS_CONFIRMED).
void settings_open_wizard(Vec3 head_pos);
void settings_close();
bool settings_is_open();
bool settings_is_wizard();
// The first launch wizard's height was set with - / + or measured: until then the floor is
// only a guess and the lobby does not show the grid.
bool settings_wizard_height_set();
// Once per lobby frame while open. rays: Move 0, Move 1, DualShock 4. Returns
// SettingsAction bits; *clicked is set to the index of the ray that clicked a button (-1
// if none), for a haptic tick.
unsigned settings_update(const SettingsContext &ctx, const SettingsRay rays[LOBBY_POINTERS], uint64_t now_us,
                         int *clicked);
// Fills the panel and the lasers for the lobby renderer.
void settings_build(LobbyPanel *panel, LobbyPointer pointers[LOBBY_POINTERS]);
