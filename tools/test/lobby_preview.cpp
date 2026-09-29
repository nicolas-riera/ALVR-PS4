// Host-side preview of the lobby renderer: writes a side-by-side PPM.
// Build (WSL):
//   g++ -O2 -I client/src tools/test/lobby_preview.cpp client/src/lobby.cpp client/src/settings.cpp -o /tmp/lobby_preview
// Usage: lobby_preview out.ppm [yaw] [mode]   mode: 0 lobby, 1 settings open, 2 headset lost (black + text),
//                                             3 close-up of the controllers (buttons, battery),
//                                             4 first launch wizard, 5 close-up of the DualShock 4
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lobby.h"
#include "move.h"
#include "pad.h"
#include "settings.h"

// Stubs for what settings.cpp uses on the console.
void log_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}
void config_store(const ClientConfig *) {}

int main(int argc, char **argv)
{
    const int W = 1920, H = 1080;
    uint32_t *px = (uint32_t *)malloc(W * H * 4);
    LobbyView v;
    memset(&v, 0, sizeof(v));
    float yaw = argc > 2 ? atof(argv[2]) : 0.0f; // radians around +Y
    int mode = argc > 3 ? atoi(argv[3]) : 0;
    Quat rot{0, sinf(yaw / 2), 0, cosf(yaw / 2)};
    Vec3 head = v3(0.0f, -0.27f, 0.9f);
    for (int eye = 0; eye < 2; eye++) {
        v.eye_pos[eye] = head + rotate(rot, v3(eye ? 0.0315f : -0.0315f, 0, 0));
        v.eye_rot[eye] = rot;
    }
    v.head_pos = head;
    v.head_rot = rot;
    v.fov[0] = EyeFov{1.2074f, 1.1813f, 1.2629f, 1.2629f};
    v.fov[1] = EyeFov{1.1813f, 1.2074f, 1.2629f, 1.2629f};
    v.floor_y = -1.47f;
    v.center_x = 0.3f;
    v.center_z = 0.9f;
    SettingsRay rays[LOBBY_POINTERS];
    memset(rays, 0, sizeof(rays));
    for (int i = 0; i < 2; i++) {
        LobbyView::Controller &c = v.controllers[i];
        c.visible = true;
        c.pos = v3(i ? 0.12f : -0.12f, -0.4f, 0.6f);
        c.rot = Quat{0.3827f, 0, 0, 0.9239f}; // tilted 45 deg around X
        c.rgb = i ? 0xff00ff : 0x00ffff;
        c.tracked = true;
        c.hand_letter = i ? 'R' : 'L';
        c.pad_touch = false;
        c.buttons = i ? (MOVE_BUTTON_TRIANGLE | MOVE_BUTTON_CIRCLE | MOVE_BUTTON_START)
                      : (MOVE_BUTTON_SQUARE | MOVE_BUTTON_CROSS | MOVE_BUTTON_MOVE | MOVE_BUTTON_SELECT);
        c.trigger = i ? 1.0f : 0.3f;
        c.battery = i ? 0.4f : 0.8f;
        c.charging = i == 1;
        if (mode == 3) { // upright, buttons towards the viewer, 25 cm away
            c.pos = v3(i ? 0.035f : -0.035f, head.y + 0.03f, head.z - 0.25f);
            c.rot = Quat{0.7071f, 0, 0, 0.7071f};
        }
        rays[i].valid = true;
        rays[i].origin = c.pos;
        rays[i].dir = rotate(Quat{0.1f, i ? -0.05f : 0.08f, 0, 0.99f}, v3(0, 0, -1));
        float n = sqrtf(rays[i].dir.x * rays[i].dir.x + rays[i].dir.y * rays[i].dir.y + rays[i].dir.z * rays[i].dir.z);
        rays[i].dir = rays[i].dir * (1.0f / n);
        rays[i].trigger = 0.0f;
    }
    // DualShock 4 held in front, top face tilted towards the viewer.
    {
        LobbyView::Pad &d = v.pads[0];
        const float a = (mode == 5 ? 55.0f : 30.0f) * 0.0174533f;
        d.visible = true;
        d.pos = mode == 5 ? head + v3(0.0f, -0.10f, -0.26f) : v3(0.0f, -0.55f, 0.45f);
        d.rot = Quat{sinf(a / 2), 0, 0, cosf(a / 2)};
        d.rgb = 0x0000ff;
        d.tracked = true;
        d.buttons = PAD_BUTTON_CROSS | PAD_BUTTON_TRIANGLE | PAD_BUTTON_LEFT | PAD_BUTTON_R1 | PAD_BUTTON_R3 |
                    PAD_BUTTON_OPTIONS | PAD_BUTTON_TOUCH_PAD;
        d.lx = -0.7f;
        d.ly = 0.7f;
        d.rx = 0.2f;
        d.l2 = 0.4f;
        d.r2 = 1.0f;
        d.touch[0] = true;
        d.touch_x[0] = 0.25f;
        d.touch_y[0] = 0.4f;
        d.touch[1] = true;
        d.touch_x[1] = 0.8f;
        d.touch_y[1] = 0.7f;
        d.battery = 0.6f;
        d.charging = true;
        d.rumble_large = 0.4f;
        d.rumble_small = 1.0f;
        v.time_s = 0.3f;
        // A second user's pad, labelled "2", further away (lobby view only).
        if (mode == 0) {
            v.pads[1] = d;
            v.pads[1].pos = v3(0.35f, -0.62f, 0.45f);
            v.pads[1].rot = Quat{sinf(0.35f), 0, 0, cosf(0.35f)};
            v.pads[1].floating = true;
            v.pads[1].tracked = false;
            v.pads[1].rgb = 0xffff00;
            v.pads[1].label = '2';
            v.pads[1].buttons = 0;
            v.pads[1].rumble_large = v.pads[1].rumble_small = 0.0f;
        }
        rays[2].valid = mode != 5;
        rays[2].origin = d.pos;
        rays[2].dir = rotate(d.rot, v3(0, 0, -1));
    }
    v.info[0] = "ALVR PS4 (Dev)";
    v.info[1] = "Waiting for the PC (ALVR streamer 20.14.1)";
    v.info[2] = "Hostname: 1234.client";
    v.info[3] = "IP: 192.168.0.124";
    v.info[4] = "Client v0.9.3";
    v.info[5] = "Press Start (PS Move) or Options (DualShock 4) to open settings";
    v.info[6] = nullptr;
    v.info_pos = v3(0.0f, v.floor_y + 1.6f, -3.0f);
    v.info_yaw = 0.0f;
    v.brightness = 1.0f;
    static LobbyPanel panel;
    if (mode == 1 || mode == 4) {
        ClientConfig cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.resolution_percent = 130;
        cfg.center_on_connect = 1;
        cfg.refresh_rate = 90;
        if (mode == 4)
            settings_open_wizard(head);
        else
            settings_open(head);
        SettingsContext ctx{&cfg, v.floor_y, head.y, 2};
        int clicked;
        settings_update(ctx, rays, 1000000, &clicked);
        settings_update(ctx, rays, 1016000, &clicked);
        settings_build(&panel, v.pointers);
        v.panel = &panel;
    }
    if (mode == 2) {
        v.brightness = 0.0f;
        v.overlay_text = "Headset not detected by PSCamera.";
        v.overlay_brightness = 1.0f;
    }
    lobby_render(px, W, H, W, &v);
    FILE *f = fopen(argc > 1 ? argv[1] : "lobby.ppm", "wb");
    fprintf(f, "P6 %d %d 255\n", W, H);
    for (int i = 0; i < W * H; i++) {
        unsigned char rgb[3] = {(unsigned char)(px[i] >> 16), (unsigned char)(px[i] >> 8), (unsigned char)px[i]};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    return 0;
}
