#include "lobby.h"

#include <string.h>

static const uint32_t PIXEL_ALPHA = 0x80000000;
static const float NEAR_Z = 0.05f;

struct EyeTarget {
    uint32_t *pixels;
    int x0, width, height, pitch;
    Vec3 eye_pos;
    Quat inv_rot;
    EyeFov fov;
};

static void plot(const EyeTarget &t, int x, int y, uint32_t c)
{
    if ((unsigned)x < (unsigned)t.width && (unsigned)y < (unsigned)t.height)
        t.pixels[(size_t)y * t.pitch + t.x0 + x] = c;
}

// Liang-Barsky clip of the segment to the eye viewport, then integer Bresenham.
static bool clip2d(float w, float h, float *x0, float *y0, float *x1, float *y1)
{
    float t0 = 0.0f, t1 = 1.0f, dx = *x1 - *x0, dy = *y1 - *y0;
    const float p[4] = {-dx, dx, -dy, dy};
    const float q[4] = {*x0, w - 1 - *x0, *y0, h - 1 - *y0};
    for (int i = 0; i < 4; i++) {
        if (p[i] == 0.0f) {
            if (q[i] < 0.0f)
                return false;
            continue;
        }
        float r = q[i] / p[i];
        if (p[i] < 0.0f) {
            if (r > t1)
                return false;
            if (r > t0)
                t0 = r;
        } else {
            if (r < t0)
                return false;
            if (r < t1)
                t1 = r;
        }
    }
    float ox = *x0, oy = *y0;
    *x0 = ox + t0 * dx;
    *y0 = oy + t0 * dy;
    *x1 = ox + t1 * dx;
    *y1 = oy + t1 * dy;
    return true;
}

static void line2d(const EyeTarget &t, float fx0, float fy0, float fx1, float fy1, uint32_t c)
{
    if (!clip2d((float)t.width, (float)t.height, &fx0, &fy0, &fx1, &fy1))
        return;
    int x0 = (int)fx0, y0 = (int)fy0, x1 = (int)fx1, y1 = (int)fy1;
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        plot(t, x0, y0, c);
        if (x0 == x1 && y0 == y1)
            break;
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

static Vec3 to_view(const EyeTarget &t, Vec3 p)
{
    return rotate(t.inv_rot, p - t.eye_pos);
}

static void project(const EyeTarget &t, Vec3 v, float *sx, float *sy)
{
    // View space: -Z forward, +Y up, +X right.
    float tx = v.x / -v.z, ty = v.y / -v.z;
    *sx = (tx + t.fov.tan_left) / (t.fov.tan_left + t.fov.tan_right) * t.width;
    *sy = (t.fov.tan_up - ty) / (t.fov.tan_up + t.fov.tan_down) * t.height;
}

static void line3d(const EyeTarget &t, Vec3 a, Vec3 b, uint32_t c)
{
    Vec3 va = to_view(t, a), vb = to_view(t, b);
    // Clip against the near plane z = -NEAR_Z.
    bool ina = va.z < -NEAR_Z, inb = vb.z < -NEAR_Z;
    if (!ina && !inb)
        return;
    if (ina != inb) {
        float k = (-NEAR_Z - va.z) / (vb.z - va.z);
        Vec3 m = va + (vb - va) * k;
        if (ina)
            vb = m;
        else
            va = m;
    }
    float x0, y0, x1, y1;
    project(t, va, &x0, &y0);
    project(t, vb, &x1, &y1);
    line2d(t, x0, y0, x1, y1, c);
}

static uint32_t grey(float intensity)
{
    int v = (int)(intensity * 255.0f);
    v = v < 0 ? 0 : v > 255 ? 255 : v;
    return PIXEL_ALPHA | v << 16 | v << 8 | v;
}

static void draw_scene(const EyeTarget &t, const LobbyView *view)
{
    // Floor grid, 1 m cells, fading with distance from the player.
    const int half = 12;
    const float y = view->floor_y;
    for (int i = -half; i <= half; i++) {
        float fade = 1.0f - (i < 0 ? -i : i) / (float)(half + 1);
        uint32_t c = grey(0.25f + 0.65f * fade * fade);
        line3d(t, v3((float)i, y, -half), v3((float)i, y, half), c);
        line3d(t, v3(-half, y, (float)i), v3(half, y, (float)i), c);
    }

    // PS Camera placeholder: a small wireframe box at the tracker origin.
    const float w = 0.10f, h = 0.03f, d = 0.03f;
    Vec3 p[8];
    for (int k = 0; k < 8; k++)
        p[k] = v3(k & 1 ? w : -w, k & 2 ? h : -h, k & 4 ? d : -d);
    static const int edges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3},
                                     {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (auto &e : edges)
        line3d(t, p[e[0]], p[e[1]], PIXEL_ALPHA | 0x40c0ff);
}

void lobby_render(uint32_t *pixels, int width, int height, int pitch, const LobbyView *view)
{
    const uint32_t bg = PIXEL_ALPHA | 0x06080c;
    for (int yy = 0; yy < height; yy++) {
        uint32_t *row = pixels + (size_t)yy * pitch;
        for (int xx = 0; xx < width; xx++)
            row[xx] = bg;
    }
    const int eye_w = width / 2;
    for (int eye = 0; eye < 2; eye++) {
        EyeTarget t;
        t.pixels = pixels;
        t.x0 = eye * eye_w;
        t.width = eye_w;
        t.height = height;
        t.pitch = pitch;
        float side = eye == 0 ? -0.5f : 0.5f;
        t.eye_pos = view->head_pos + rotate(view->head_rot, v3(side * view->ipd, 0, 0));
        t.inv_rot = conj(view->head_rot);
        t.fov = view->fov[eye];
        draw_scene(t, view);
    }
}
