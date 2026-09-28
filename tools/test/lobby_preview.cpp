// Host-side preview of the lobby renderer: writes a side-by-side PPM.
// Build (WSL): g++ -O2 -I client/src tools/test/lobby_preview.cpp client/src/lobby.cpp -o /tmp/lobby_preview
#include <stdio.h>
#include <stdlib.h>
#include "lobby.h"

int main(int argc, char **argv)
{
    const int W = 1920, H = 1080;
    uint32_t *px = (uint32_t *)malloc(W * H * 4);
    LobbyView v;
    v.head_pos = v3(0.0f, -0.27f, 0.9f);
    float yaw = argc > 2 ? atof(argv[2]) : 0.0f; // radians around +Y
    v.head_rot = Quat{0, sinf(yaw / 2), 0, cosf(yaw / 2)};
    v.ipd = 0.063f;
    v.fov[0] = EyeFov{1.2074f, 1.1813f, 1.2629f, 1.2629f};
    v.fov[1] = EyeFov{1.1813f, 1.2074f, 1.2629f, 1.2629f};
    v.floor_y = -1.47f;
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
