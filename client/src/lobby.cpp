#include "lobby.h"

#include <math.h>
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

static inline void plot_aa(const EyeTarget &t, int x, int y, uint32_t c, float a)
{
    if ((unsigned)x >= (unsigned)t.width || (unsigned)y >= (unsigned)t.height)
        return;
    uint32_t &d = t.pixels[(size_t)y * t.pitch + t.x0 + x];
    uint32_t out = PIXEL_ALPHA;
    for (int sh = 0; sh <= 16; sh += 8) {
        uint32_t src = (uint32_t)(((c >> sh) & 0xff) * a);
        uint32_t dst = (d >> sh) & 0xff;
        out |= (src > dst ? src : dst) << sh;
    }
    d = out;
}

static inline float fpart(float x) { return x - floorf(x); }

// Xiaolin Wu anti-aliased line, after clipping to the viewport; max-blended.
static void line2d(const EyeTarget &t, float x0, float y0, float x1, float y1, uint32_t c)
{
    if (!clip2d((float)t.width, (float)t.height, &x0, &y0, &x1, &y1))
        return;
    bool steep = fabsf(y1 - y0) > fabsf(x1 - x0);
    if (steep) {
        float tmp = x0; x0 = y0; y0 = tmp;
        tmp = x1; x1 = y1; y1 = tmp;
    }
    if (x0 > x1) {
        float tmp = x0; x0 = x1; x1 = tmp;
        tmp = y0; y0 = y1; y1 = tmp;
    }
    float dx = x1 - x0, dy = y1 - y0;
    float grad = dx < 1e-6f ? 1.0f : dy / dx;
    float y = y0 + grad * (roundf(x0) - x0);
    for (int x = (int)roundf(x0); x <= (int)roundf(x1); x++, y += grad) {
        int yi = (int)floorf(y);
        float f = fpart(y);
        if (steep) {
            plot_aa(t, yi, x, c, 1.0f - f);
            plot_aa(t, yi + 1, x, c, f);
        } else {
            plot_aa(t, x, yi, c, 1.0f - f);
            plot_aa(t, x, yi + 1, c, f);
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

static void circle3d(const EyeTarget &t, Vec3 c, Vec3 u, Vec3 v, float r, uint32_t col)
{
    const int n = 20;
    Vec3 prev = c + u * r;
    for (int i = 1; i <= n; i++) {
        float a = 6.2831853f * i / n;
        Vec3 p = c + u * (r * cosf(a)) + v * (r * sinf(a));
        line3d(t, prev, p, col);
        prev = p;
    }
}

// Placeholder PS Move until real 3D models are in: the sphere as three great circles
// and a box handle along the local +Z axis.
static void draw_controller(const EyeTarget &t, const LobbyView::Controller &c)
{
    const float r = 0.0225f;
    Vec3 x = rotate(c.rot, v3(1, 0, 0)), y = rotate(c.rot, v3(0, 1, 0)), z = rotate(c.rot, v3(0, 0, 1));
    uint32_t col = PIXEL_ALPHA | (c.tracked ? c.rgb : 0x606060);
    circle3d(t, c.pos, x, y, r, col);
    circle3d(t, c.pos, y, z, r, col);
    circle3d(t, c.pos, z, x, r, col);
    const float hw = 0.018f, z0 = r, z1 = r + 0.16f;
    Vec3 p[8];
    for (int k = 0; k < 8; k++)
        p[k] = c.pos + x * (k & 1 ? hw : -hw) + y * (k & 2 ? hw : -hw) + z * (k & 4 ? z1 : z0);
    static const int edges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3},
                                     {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (auto &e : edges)
        line3d(t, p[e[0]], p[e[1]], PIXEL_ALPHA | 0xb0b0b0);
}

void lobby_render_eye(uint32_t *pixels, int width, int height, int pitch, const LobbyView *view, int eye)
{
    const uint32_t bg = PIXEL_ALPHA | 0x06080c;
    const uint64_t bg2 = (uint64_t)bg << 32 | bg;
    for (int yy = 0; yy < height; yy++) {
        uint64_t *row = (uint64_t *)(pixels + (size_t)yy * pitch);
        for (int xx = 0; xx < width / 2; xx++)
            row[xx] = bg2;
    }
    EyeTarget t;
    t.pixels = pixels;
    t.x0 = 0;
    t.width = width;
    t.height = height;
    t.pitch = pitch;
    t.eye_pos = view->eye_pos[eye];
    t.inv_rot = conj(view->eye_rot[eye]);
    t.fov = view->fov[eye];
    draw_scene(t, view);
    for (auto &c : view->controllers)
        if (c.visible)
            draw_controller(t, c);
}

void lobby_render(uint32_t *pixels, int width, int height, int pitch, const LobbyView *view)
{
    const int eye_w = width / 2;
    lobby_render_eye(pixels, eye_w, height, pitch, view, 0);
    lobby_render_eye(pixels + eye_w, eye_w, height, pitch, view, 1);
}
