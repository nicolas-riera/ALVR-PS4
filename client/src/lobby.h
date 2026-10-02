#pragma once

#include <stdint.h>

#include "vrmath.h"

// Lobby environment shown while no PC is streaming: a floor grid like the
// official ALVR client, plus a marker at the PS Camera. Rendered in software
// into a side-by-side stereo image (left eye in the left half).

struct EyeFov {
    float tan_left, tan_right, tan_up, tan_down;
};

// A flat panel in the scene (the settings), made of rectangles and text laid out in
// panel coordinates: metres from the panel centre, x right, y up.
struct LobbyPanelItem {
    float x0, y0, x1, y1;
    uint32_t fill;    // RGB, 0 = not filled
    uint32_t outline; // RGB, 0 = no outline
    const char *text; // drawn inside the rectangle, or nullptr
    float text_h;     // cap height, metres
    int align;        // -1 left, 0 centre, 1 right (with a small margin)
    uint32_t text_rgb;
};

// Line drawing on a panel (pictograms), drawn over the items: a segment from (x0, y0) to
// (x1, y1), or a circle of radius r centred on (x0, y0) when r > 0.
struct LobbyPanelShape {
    float x0, y0, x1, y1, r;
    uint32_t rgb;
};

#define LOBBY_PANEL_MAX_ITEMS 72
#define LOBBY_PANEL_MAX_SHAPES 64

struct LobbyPanel {
    bool visible;
    Vec3 origin;     // centre, tracker space
    Vec3 right, up;  // unit axes
    int count;
    LobbyPanelItem items[LOBBY_PANEL_MAX_ITEMS];
    int shape_count;
    LobbyPanelShape shapes[LOBBY_PANEL_MAX_SHAPES];
};

// Settings lasers: the two PS Moves and the DualShock 4 of the user playing.
#define LOBBY_POINTERS 3
// Controllers shown: two PS Moves and one DualShock 4 per logged-in user (4 at most).
#define LOBBY_CONTROLLERS 8
#define LOBBY_PADS 4

struct LobbyPointer {
    bool visible;
    Vec3 from, to;   // laser segment, tracker space
    bool hit;        // ends on the panel: a cursor is drawn at `to`
    uint32_t rgb;
};

struct LobbyView {
    Vec3 eye_pos[2]; // tracker space: origin at the PS Camera, metres
    Quat eye_rot[2];
    EyeFov fov[2];  // left, right
    float floor_y;  // tracker-space height of the floor
    bool grid_visible; // floor grid shown (not while the floor is only a guess)
    float center_x, center_z; // play space centre: the grid is aligned on it
    struct Controller {
        bool visible;
        Vec3 pos;      // sphere centre, tracker space
        Quat rot;
        uint32_t rgb;  // sphere colour
        bool tracked;  // position currently seen by the camera
        char hand_letter; // 'L' / 'R' (user playing) or the user number '2'..'4', drawn on the handle
        bool pad_touch, pad_click;
        float pad_x, pad_y;
        uint16_t buttons; // MoveButton bits: pressed buttons are drawn where they are
        float trigger;    // 0..1, drawn while pressed
        float battery;    // 0..1, < 0 unknown (drawn under the hand letter)
        bool charging;
    } controllers[LOBBY_CONTROLLERS];
    // DualShock 4, tracked by its light bar. Pad frame: x right, y up (touchpad side), -z
    // forward (light bar side), origin at the light bar.
    struct Pad {
        bool visible;
        Vec3 pos;
        Quat rot;
        uint32_t rgb;     // light bar colour
        bool tracked;
        uint32_t buttons; // PadButton bits
        float lx, ly, rx, ry; // sticks, -1..1 (+y up)
        float l2, r2;         // 0..1
        bool touch[2];
        float touch_x[2], touch_y[2]; // 0..1 from the touchpad's top left corner
        float battery;        // 0..1, < 0 unknown
        bool charging;
        float rumble_large, rumble_small; // motors, 0..1
        char label;           // 0 for the user playing, else the user number '2'..'4'
        bool floating;        // not tracked: shown still in front of the play area
    } pads[LOBBY_PADS];
    // The PS VR headset as a wireframe: only for ALVR PS4 Tracking Viewer on the PC (never in the
    // headset itself). Pose of the centre between the eyes, -Z forward.
    struct Headset {
        bool visible;
        Vec3 pos;
        Quat rot;
        bool tracked;
    } headset;
    float time_s;           // for animations (rumble)
    const char *info[8];    // info panel lines (nullptr-terminated)
    Vec3 info_pos;          // panel centre, tracker space
    float info_yaw;         // panel facing (radians around +Y; 0 faces +Z)
    LobbyPanel *panel;      // settings panel, or nullptr
    LobbyPointer pointers[LOBBY_POINTERS];
    // Fade of the surroundings (grid, info panel, settings): 1 = fully visible, 0 = black.
    // The PS Camera and the controllers stay visible.
    float brightness;
    // Only the PS Camera, drawn at beacon_pos in beacon_rgb (headset tracking not started:
    // the rest is hidden, the camera shows where to look).
    bool beacon;
    Vec3 beacon_pos;
    uint32_t beacon_rgb;
    // Text attached to the view (shown over the black), with its own brightness.
    const char *overlay_text;
    float overlay_brightness;
    Vec3 head_pos; // centre between the eyes, for the attached text
    Quat head_rot;
    // Fade of the whole picture to black, the controllers and the attached text included
    // (switching between the lobby and the stream): 0 = none, 1 = black.
    float black;
};

// One eye (0 = left, 1 = right) into its own image.
void lobby_render_eye(uint32_t *pixels, int width, int height, int pitch, const LobbyView *view, int eye);
// Both eyes side by side (left half / right half), used by the host preview.
void lobby_render(uint32_t *pixels, int width, int height, int pitch, const LobbyView *view);
// Width of a stroke-font string of the given cap height (metres).
float lobby_text_width(const char *s, float height);
