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

#define LOBBY_PANEL_MAX_ITEMS 48

struct LobbyPanel {
    bool visible;
    Vec3 origin;     // centre, tracker space
    Vec3 right, up;  // unit axes
    int count;
    LobbyPanelItem items[LOBBY_PANEL_MAX_ITEMS];
};

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
    float center_x, center_z; // play space centre: the grid is aligned on it
    struct Controller {
        bool visible;
        Vec3 pos;      // sphere centre, tracker space
        Quat rot;
        uint32_t rgb;  // sphere colour
        bool tracked;  // position currently seen by the camera
        char hand_letter; // 'L' / 'R', drawn on the handle
        bool pad_touch, pad_click;
        float pad_x, pad_y;
        uint16_t buttons; // MoveButton bits: pressed buttons are drawn where they are
        float trigger;    // 0..1, drawn while pressed
        float battery;    // 0..1, < 0 unknown (drawn under the hand letter)
        bool charging;
    } controllers[2];
    const char *info[8];    // info panel lines (nullptr-terminated)
    Vec3 info_pos;          // panel centre, tracker space
    float info_yaw;         // panel facing (radians around +Y; 0 faces +Z)
    LobbyPanel *panel;      // settings panel, or nullptr
    LobbyPointer pointers[2];
    // Fade to black: 1 = scene fully visible, 0 = black.
    float brightness;
    // Text attached to the view (shown over the black), with its own brightness.
    const char *overlay_text;
    float overlay_brightness;
    Vec3 head_pos; // centre between the eyes, for the attached text
    Quat head_rot;
};

// One eye (0 = left, 1 = right) into its own image.
void lobby_render_eye(uint32_t *pixels, int width, int height, int pitch, const LobbyView *view, int eye);
// Both eyes side by side (left half / right half), used by the host preview.
void lobby_render(uint32_t *pixels, int width, int height, int pitch, const LobbyView *view);
// Width of a stroke-font string of the given cap height (metres).
float lobby_text_width(const char *s, float height);
