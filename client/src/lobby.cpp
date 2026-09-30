#include "lobby.h"

#include "move.h"
#include "pad.h"
#include "stroke_font.h"

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

// Convex polygon in view space: clipped against the near plane, projected, then filled
// row by row (solid colour, overwriting what is behind).
static void fill_poly_view(const EyeTarget &t, const Vec3 *in, int n, uint32_t rgb)
{
    Vec3 clipped[12];
    int m = 0;
    for (int i = 0; i < n && m < 10; i++) {
        Vec3 a = in[i], b = in[(i + 1) % n];
        bool ina = a.z < -NEAR_Z, inb = b.z < -NEAR_Z;
        if (ina)
            clipped[m++] = a;
        if (ina != inb) {
            float k = (-NEAR_Z - a.z) / (b.z - a.z);
            clipped[m++] = a + (b - a) * k;
        }
    }
    if (m < 3)
        return;
    float sx[12], sy[12], ymin = 1e9f, ymax = -1e9f;
    for (int i = 0; i < m; i++) {
        project(t, clipped[i], &sx[i], &sy[i]);
        ymin = sy[i] < ymin ? sy[i] : ymin;
        ymax = sy[i] > ymax ? sy[i] : ymax;
    }
    int y0 = (int)ceilf(ymin - 0.5f), y1 = (int)floorf(ymax - 0.5f);
    if (y0 < 0)
        y0 = 0;
    if (y1 > t.height - 1)
        y1 = t.height - 1;
    const uint32_t c = PIXEL_ALPHA | rgb;
    for (int y = y0; y <= y1; y++) {
        float yc = y + 0.5f, xl = 1e9f, xr = -1e9f;
        for (int i = 0; i < m; i++) {
            int j = (i + 1) % m;
            float ya = sy[i], yb = sy[j];
            if ((ya <= yc && yb > yc) || (yb <= yc && ya > yc)) {
                float x = sx[i] + (yc - ya) / (yb - ya) * (sx[j] - sx[i]);
                xl = x < xl ? x : xl;
                xr = x > xr ? x : xr;
            }
        }
        int x0 = (int)ceilf(xl - 0.5f), x1 = (int)floorf(xr - 0.5f);
        if (x0 < 0)
            x0 = 0;
        if (x1 > t.width - 1)
            x1 = t.width - 1;
        uint32_t *row = t.pixels + (size_t)y * t.pitch + t.x0;
        for (int x = x0; x <= x1; x++)
            row[x] = c;
    }
}

static void fill_quad3d(const EyeTarget &t, Vec3 a, Vec3 b, Vec3 c, Vec3 d, uint32_t rgb)
{
    const Vec3 q[4] = {to_view(t, a), to_view(t, b), to_view(t, c), to_view(t, d)};
    fill_poly_view(t, q, 4, rgb);
}

static void draw_grid(const EyeTarget &t, const LobbyView *view)
{
    // Floor grid, 1 m cells aligned on the play space centre, fading with distance.
    const int half = 12;
    const float y = view->floor_y, cx = view->center_x, cz = view->center_z;
    for (int i = -half; i <= half; i++) {
        float fade = 1.0f - (i < 0 ? -i : i) / (float)(half + 1);
        uint32_t c = grey(0.25f + 0.65f * fade * fade);
        line3d(t, v3(cx + i, y, cz - half), v3(cx + i, y, cz + half), c);
        line3d(t, v3(cx - half, y, cz + i), v3(cx + half, y, cz + i), c);
    }
}

// PS Camera placeholder: a small wireframe box (the tracker origin is the camera).
static void draw_camera(const EyeTarget &t, Vec3 at, uint32_t rgb)
{
    const float w = 0.10f, h = 0.03f, d = 0.03f;
    Vec3 p[8];
    for (int k = 0; k < 8; k++)
        p[k] = at + v3(k & 1 ? w : -w, k & 2 ? h : -h, k & 4 ? d : -d);
    static const int edges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3},
                                     {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (auto &e : edges)
        line3d(t, p[e[0]], p[e[1]], PIXEL_ALPHA | rgb);
}

// Stroke text on a plane: origin at the baseline start, `right`/`up` unit axes.
float lobby_text_width(const char *s, float height)
{
    const float scale = height / 0.662f; // cap height of the font in em
    float w = 0;
    for (; *s; s++) {
        unsigned c = (unsigned char)*s;
        w += (c >= 32 && c <= 126 ? stroke_font_glyphs[c - 32].advance : 0.5f) * scale;
    }
    return w;
}

static void draw_text3d(const EyeTarget &t, Vec3 origin, Vec3 right, Vec3 up, float height, const char *s,
                        uint32_t col)
{
    const float scale = height / 0.662f;
    float x = 0;
    for (; *s; s++) {
        unsigned c = (unsigned char)*s;
        if (c < 32 || c > 126)
            c = '?';
        const StrokeGlyph &g = stroke_font_glyphs[c - 32];
        for (int i = 0; i < g.count; i++) {
            const float *seg = stroke_font_segs[g.first + i];
            Vec3 a = origin + right * (x + seg[0] * scale) + up * (seg[1] * scale);
            Vec3 b = origin + right * (x + seg[2] * scale) + up * (seg[3] * scale);
            line3d(t, a, b, col);
        }
        x += g.advance * scale;
    }
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

// Button symbols on the face of the Move: centre c, face axes u (right) and v (towards
// the sphere), half size s.
static void symbol_triangle(const EyeTarget &t, Vec3 c, Vec3 u, Vec3 v, float s, uint32_t col)
{
    Vec3 a = c + v * s, b = c - u * (s * 0.95f) - v * (s * 0.7f), d = c + u * (s * 0.95f) - v * (s * 0.7f);
    line3d(t, a, b, col);
    line3d(t, b, d, col);
    line3d(t, d, a, col);
}

static void symbol_square(const EyeTarget &t, Vec3 c, Vec3 u, Vec3 v, float s, uint32_t col)
{
    s *= 0.8f;
    Vec3 p[4] = {c - u * s - v * s, c + u * s - v * s, c + u * s + v * s, c - u * s + v * s};
    for (int i = 0; i < 4; i++)
        line3d(t, p[i], p[(i + 1) % 4], col);
}

static void symbol_cross(const EyeTarget &t, Vec3 c, Vec3 u, Vec3 v, float s, uint32_t col)
{
    s *= 0.8f;
    line3d(t, c - u * s - v * s, c + u * s + v * s, col);
    line3d(t, c - u * s + v * s, c + u * s - v * s, col);
}

static void ellipse3d(const EyeTarget &t, Vec3 c, Vec3 u, Vec3 v, float a, float b, uint32_t col)
{
    const int n = 24;
    Vec3 prev = c + u * a;
    for (int i = 1; i <= n; i++) {
        float ang = 6.2831853f * i / n;
        Vec3 p = c + u * (a * cosf(ang)) + v * (b * sinf(ang));
        line3d(t, prev, p, col);
        prev = p;
    }
}

// A small rectangle (side buttons): centre c, half extents along a and b.
static void rect3d(const EyeTarget &t, Vec3 c, Vec3 a, Vec3 b, uint32_t col)
{
    Vec3 p[4] = {c - a - b, c + a - b, c + a + b, c - a + b};
    for (int i = 0; i < 4; i++)
        line3d(t, p[i], p[(i + 1) % 4], col);
}

// Battery as four dots like SteamVR's, centred on bc along x (on the face spanned by x and
// z): red when one or none is left, a bolt on the right while charging.
static void draw_battery(const EyeTarget &t, Vec3 bc, Vec3 x, Vec3 z, float battery, bool charging)
{
    // Nearest number of dots, as SteamVR's gauge (a PS Move at 80 % shows 3).
    const int dots = (int)lroundf(battery * 4.0f);
    const float dr = 0.0017f, gap = 0.0052f;
    const uint32_t on = dots <= 1 ? 0xe04040 : 0xffffff;
    for (int k = 0; k < 4; k++) {
        const Vec3 dc = bc + x * ((k - 1.5f) * gap);
        if (k < dots) {
            Vec3 poly[8];
            for (int j = 0; j < 8; j++) {
                float ang = 6.2831853f * j / 8;
                poly[j] = to_view(t, dc + x * (dr * cosf(ang)) + z * (dr * sinf(ang)));
            }
            fill_poly_view(t, poly, 8, on);
        } else {
            circle3d(t, dc, x, z, dr, PIXEL_ALPHA | 0x606060);
        }
    }
    if (charging) {
        const Vec3 o = bc + x * (2.5f * gap);
        const uint32_t bolt = PIXEL_ALPHA | 0xffe040;
        line3d(t, o + x * 0.0012f - z * 0.0028f, o - x * 0.0010f + z * 0.0002f, bolt);
        line3d(t, o - x * 0.0010f + z * 0.0002f, o + x * 0.0010f - z * 0.0002f, bolt);
        line3d(t, o + x * 0.0010f - z * 0.0002f, o - x * 0.0012f + z * 0.0028f, bolt);
    }
}

// PS Move as wireframe: the sphere as three great circles and a box handle along the
// local +Z axis (buttons on the +Y face, START on the +X side, SELECT on the -X side).
static void draw_controller(const EyeTarget &t, const LobbyView::Controller &c)
{
    const float r = 0.0225f;
    Vec3 x = rotate(c.rot, v3(1, 0, 0)), y = rotate(c.rot, v3(0, 1, 0)), z = rotate(c.rot, v3(0, 0, 1));
    uint32_t col = PIXEL_ALPHA | (c.tracked ? c.rgb : 0x606060);
    circle3d(t, c.pos, x, y, r, col);
    circle3d(t, c.pos, y, z, r, col);
    circle3d(t, c.pos, z, x, r, col);
    const float hw = 0.021f, z0 = r, z1 = r + 0.16f;
    Vec3 p[8];
    for (int k = 0; k < 8; k++)
        p[k] = c.pos + x * (k & 1 ? hw : -hw) + y * (k & 2 ? hw : -hw) + z * (k & 4 ? z1 : z0);
    static const int edges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3},
                                     {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (auto &e : edges)
        line3d(t, p[e[0]], p[e[1]], PIXEL_ALPHA | 0xb0b0b0);

    // Hand letter on the lower middle of the handle (buttons side).
    if (c.hand_letter) {
        const char txt[2] = {c.hand_letter, 0};
        const float lh = 0.018f;
        Vec3 o = c.pos + z * (r + 0.115f) + y * (hw + 0.002f) + x * (-lobby_text_width(txt, lh) * 0.5f);
        draw_text3d(t, o, x, z * -1.0f, lh, txt, PIXEL_ALPHA | 0xffffff);
    }

    // Battery under the letter (the Move reports 20 % steps).
    if (c.battery >= 0.0f)
        draw_battery(t, c.pos + z * (r + 0.132f) + y * (hw + 0.002f), x, z, c.battery, c.charging);

    // Pressed buttons, drawn where they sit (measured on a CECH-ZCM1 photo): the Move button
    // in the middle, square / triangle above left / right, cross / circle below, START on
    // the right side, SELECT on the left side.
    const Vec3 face = c.pos + y * (hw + 0.002f), up = z * -1.0f;
    const float zc = 0.062f, dz = 0.0086f, dx = 0.0155f, s = 0.0036f;
    const uint16_t b = c.buttons;
    if (b & MOVE_BUTTON_SQUARE)
        symbol_square(t, face + x * -dx + z * (zc - dz), x, up, s, PIXEL_ALPHA | 0xff80d0);
    if (b & MOVE_BUTTON_TRIANGLE)
        symbol_triangle(t, face + x * dx + z * (zc - dz), x, up, s, PIXEL_ALPHA | 0x40e0a0);
    if (b & MOVE_BUTTON_CROSS)
        symbol_cross(t, face + x * -dx + z * (zc + dz), x, up, s, PIXEL_ALPHA | 0x60a0ff);
    if (b & MOVE_BUTTON_CIRCLE)
        circle3d(t, face + x * dx + z * (zc + dz), x, up, s * 0.85f, PIXEL_ALPHA | 0xff5050);
    if (b & MOVE_BUTTON_MOVE) {
        ellipse3d(t, face + z * zc, x, up, 0.0068f, 0.0120f, PIXEL_ALPHA | 0xffffff);
        ellipse3d(t, face + z * zc, x, up, 0.0042f, 0.0085f, PIXEL_ALPHA | 0xffffff);
    }
    if (b & MOVE_BUTTON_START)
        rect3d(t, c.pos + x * (hw + 0.002f) + z * (r + 0.075f), z * 0.007f, y * 0.0025f, PIXEL_ALPHA | 0xffffff);
    if (b & MOVE_BUTTON_SELECT)
        rect3d(t, c.pos + x * -(hw + 0.002f) + z * (r + 0.075f), z * 0.007f, y * 0.0025f, PIXEL_ALPHA | 0xffffff);

    // Trigger (T), under the sphere on the back, only while pressed: a T whose stem
    // follows the trigger angle, from sticking out (released) towards the handle.
    if (c.trigger > 0.02f) {
        const float a = (35.0f + 40.0f * c.trigger) * 0.0174533f;
        Vec3 pivot = c.pos - y * hw + z * (r + 0.010f);
        Vec3 dir = y * -cosf(a) + z * sinf(a);
        Vec3 tip = pivot + dir * 0.032f;
        uint32_t tc = c.trigger >= 0.9f ? 0xffd040 : grey(0.45f + 0.55f * c.trigger) & 0xffffff;
        line3d(t, pivot, tip, PIXEL_ALPHA | tc);
        line3d(t, tip - x * 0.012f, tip + x * 0.012f, PIXEL_ALPHA | tc);
    }

    // Emulated trackpad: a ring on top of the handle, with the touch point.
    if (c.pad_touch) {
        const float pr = 0.022f;
        Vec3 centre = c.pos + z * (r + 0.045f) + y * (hw + 0.004f);
        circle3d(t, centre, x, z * -1.0f, pr, PIXEL_ALPHA | 0xffffff);
        Vec3 dot = centre + x * (c.pad_x * pr) + z * (-c.pad_y * pr);
        float dr = c.pad_click ? 0.006f : 0.003f;
        circle3d(t, dot, x, z, dr, PIXEL_ALPHA | (c.pad_click ? 0xffd040 : 0xffffff));
    }
}

// DualShock 4 seen from above, right half (x >= 0, z towards the user, metres, origin at the
// light bar in the middle of the front edge), measured on a product photo (162 mm wide);
// mirrored for the left half. Few vertices on purpose: blocky, like the PS Move models.
static const float DS4_OUTLINE[][2] = {
    {0.0000f, 0.0000f}, {0.0286f, 0.0000f}, {0.0400f, 0.0010f}, {0.0610f, 0.0030f}, {0.0715f, 0.0070f},
    {0.0785f, 0.0150f}, {0.0812f, 0.0260f}, {0.0815f, 0.0465f}, {0.0795f, 0.0630f}, {0.0762f, 0.0820f},
    {0.0712f, 0.0945f}, {0.0648f, 0.1005f}, {0.0565f, 0.0995f}, {0.0502f, 0.0925f}, {0.0440f, 0.0780f},
    {0.0378f, 0.0685f}, {0.0250f, 0.0672f}, {0.0000f, 0.0676f},
};
static const int DS4_OUTLINE_N = sizeof(DS4_OUTLINE) / sizeof(DS4_OUTLINE[0]);

// Box from 8 corners: bottom face 0..3, top face 4..7 (same order).
static void box3d(const EyeTarget &t, const Vec3 p[8], uint32_t col)
{
    for (int i = 0; i < 4; i++) {
        line3d(t, p[i], p[(i + 1) % 4], col);
        line3d(t, p[4 + i], p[4 + (i + 1) % 4], col);
        line3d(t, p[i], p[4 + i], col);
    }
}

// Prism of a regular polygon (n sides): centre c on the base, axis h (height vector), in
// the plane of u / v, radius r.
static void prism3d(const EyeTarget &t, Vec3 c, Vec3 u, Vec3 v, Vec3 h, float r, int n, uint32_t col)
{
    for (int i = 0; i < n; i++) {
        const float a0 = 6.2831853f * (i + 0.5f) / n, a1 = 6.2831853f * (i + 1.5f) / n;
        const Vec3 p0 = c + u * (r * cosf(a0)) + v * (r * sinf(a0)), p1 = c + u * (r * cosf(a1)) + v * (r * sinf(a1));
        line3d(t, p0, p1, col);
        line3d(t, p0 + h, p1 + h, col);
        line3d(t, p0, p0 + h, col);
    }
}

// DualShock 4 as blocky wireframe: the body as a prism of its outline (the grips slope
// down), a raised touchpad with the light bar in its tracker colour along the front, sticks
// as octagonal prisms that move with the stick, the D-pad as a cross, the face buttons,
// L1 / R1 and L2 / R2 as boxes (triggers tilt with the analog value), every button dim and
// lit while pressed, rumble as waves around the grips, battery dots, and the user number
// of another user's pad.
static void draw_pad(const EyeTarget &t, const LobbyView::Pad &p, float time_s)
{
    const Vec3 X = rotate(p.rot, v3(1, 0, 0)), Y = rotate(p.rot, v3(0, 1, 0)), Z = rotate(p.rot, v3(0, 0, 1));
    auto at = [&](float x, float y, float z) { return p.pos + X * x + Y * y + Z * z; };
    const uint32_t body = PIXEL_ALPHA | (p.tracked ? 0xb0b0b0 : 0x606060), dim = PIXEL_ALPHA | 0x505860;
    const uint32_t lit = PIXEL_ALPHA | 0xffffff, gold = PIXEL_ALPHA | 0xffd040;
    // Top and bottom of the body along z: flat over the middle, the grips going down.
    auto top_y = [](float z) { return 0.004f - (z > 0.060f ? (z - 0.060f) * 0.30f : 0.0f); };
    auto bottom_y = [](float z) { return -0.022f - (z > 0.045f ? (z - 0.045f) * 0.42f : 0.0f); };
    const float face = 0.0055f; // the parts on the flat top

    // Body: top and bottom outlines, and every vertical edge.
    for (int side = -1; side <= 1; side += 2) {
        for (int i = 0; i < DS4_OUTLINE_N; i++) {
            const float *a = DS4_OUTLINE[i];
            const Vec3 ta = at(side * a[0], top_y(a[1]), a[1]), ba = at(side * a[0], bottom_y(a[1]), a[1]);
            if (a[0] > 0.0f)
                line3d(t, ta, ba, body);
            if (i + 1 < DS4_OUTLINE_N) {
                const float *b = DS4_OUTLINE[i + 1];
                line3d(t, ta, at(side * b[0], top_y(b[1]), b[1]), body);
                line3d(t, ba, at(side * b[0], bottom_y(b[1]), b[1]), body);
            }
        }
    }

    // Touchpad (54 x 33 mm) as a low box; clicked: gold. Light bar on its front edge.
    const float tx = 0.0272f, tz0 = 0.0005f, tz1 = 0.0330f;
    const bool pad_click = (p.buttons & PAD_BUTTON_TOUCH_PAD) != 0;
    {
        const uint32_t c = pad_click ? gold : body;
        const Vec3 q[8] = {at(-tx, 0.004f, tz0), at(tx, 0.004f, tz0), at(tx, 0.004f, tz1), at(-tx, 0.004f, tz1),
                           at(-tx, face, tz0),   at(tx, face, tz0),   at(tx, face, tz1),   at(-tx, face, tz1)};
        box3d(t, q, c);
        Vec3 lb[4] = {to_view(t, at(-0.022f, 0.0035f, -0.0005f)), to_view(t, at(0.022f, 0.0035f, -0.0005f)),
                      to_view(t, at(0.022f, -0.004f, -0.0005f)), to_view(t, at(-0.022f, -0.004f, -0.0005f))};
        fill_poly_view(t, lb, 4, p.tracked ? p.rgb : 0x404040);
        for (int i = 0; i < 2; i++) {
            if (!p.touch[i])
                continue;
            const Vec3 c2 = at(-tx + 2 * tx * p.touch_x[i], face + 0.0005f, tz0 + (tz1 - tz0) * p.touch_y[i]);
            circle3d(t, c2, X, Z, 0.0035f, lit);
            circle3d(t, c2, X, Z, 0.0012f, lit);
        }
        if (p.label) {
            const char txt[2] = {p.label, 0};
            const float h = 0.012f;
            draw_text3d(t, at(-lobby_text_width(txt, h) * 0.5f, face + 0.0005f, 0.5f * (tz0 + tz1) + h * 0.5f), X,
                        Z * -1.0f, h, txt, PIXEL_ALPHA | 0xffffff);
        }
    }

    // D-pad: a cross outline, each arm lit while pressed.
    const float dx = -0.0525f, dz = 0.0271f, arm = 0.0095f, hw = 0.0036f;
    {
        struct Arm { uint32_t bit; float ux, uz; };
        static const Arm arms[4] = {{PAD_BUTTON_UP, 0, -1}, {PAD_BUTTON_RIGHT, 1, 0}, {PAD_BUTTON_DOWN, 0, 1},
                                    {PAD_BUTTON_LEFT, -1, 0}};
        for (const Arm &a : arms) {
            const Vec3 dir = X * a.ux + Z * a.uz, side = X * -a.uz + Z * a.ux;
            const Vec3 c = at(dx, face, dz);
            const uint32_t col = (p.buttons & a.bit) ? lit : dim;
            const Vec3 in0 = c + dir * hw + side * hw, in1 = c + dir * hw - side * hw;
            const Vec3 out0 = c + dir * arm + side * hw, out1 = c + dir * arm - side * hw;
            line3d(t, in0, out0, col);
            line3d(t, out0, out1, col);
            line3d(t, out1, in1, col);
        }
    }

    // Face buttons around (55, 27) mm: triangle up, circle right, cross down, square left.
    const float bx = 0.0551f, bz = 0.0265f, off = 0.0121f, br = 0.0056f, s = 0.0034f;
    const Vec3 fwd = Z * -1.0f; // "up" on the face, towards the light bar
    const Vec3 tri = at(bx, face, bz - off), cir = at(bx + off, face, bz), crs = at(bx, face, bz + off),
               sqr = at(bx - off, face, bz);
    const bool on_tri = p.buttons & PAD_BUTTON_TRIANGLE, on_cir = p.buttons & PAD_BUTTON_CIRCLE,
               on_crs = p.buttons & PAD_BUTTON_CROSS, on_sqr = p.buttons & PAD_BUTTON_SQUARE;
    prism3d(t, tri, X, Z, Y * 0.002f, br, 8, on_tri ? lit : dim);
    prism3d(t, cir, X, Z, Y * 0.002f, br, 8, on_cir ? lit : dim);
    prism3d(t, crs, X, Z, Y * 0.002f, br, 8, on_crs ? lit : dim);
    prism3d(t, sqr, X, Z, Y * 0.002f, br, 8, on_sqr ? lit : dim);
    const Vec3 up2 = Y * 0.0021f;
    symbol_triangle(t, tri + up2, X, fwd, s, PIXEL_ALPHA | (on_tri ? 0x40e0a0 : 0x306050));
    circle3d(t, cir + up2, X, Z, s * 0.85f, PIXEL_ALPHA | (on_cir ? 0xff5050 : 0x703030));
    symbol_cross(t, crs + up2, X, fwd, s, PIXEL_ALPHA | (on_crs ? 0x60a0ff : 0x304870));
    symbol_square(t, sqr + up2, X, fwd, s, PIXEL_ALPHA | (on_sqr ? 0xff80d0 : 0x703860));

    // Sticks: an octagonal base ring and a cap prism moved by the stick (L3 / R3: gold).
    for (int side = -1; side <= 1; side += 2) {
        const float sx = side < 0 ? p.lx : p.rx, sy = side < 0 ? p.ly : p.ry;
        const bool click = (p.buttons & (side < 0 ? PAD_BUTTON_L3 : PAD_BUTTON_R3)) != 0;
        const Vec3 base = at(side * 0.0263f, face, 0.0490f);
        prism3d(t, base, X, Z, Y * 0.0015f, 0.0125f, 8, dim);
        const Vec3 cap = base + X * (sx * 0.0045f) + fwd * (sy * 0.0045f) + Y * (click ? 0.003f : 0.005f);
        const bool moved = sx * sx + sy * sy > 0.01f;
        prism3d(t, cap, X, Z, Y * 0.004f, 0.0088f, 8, click ? gold : moved ? lit : body);
    }

    // SHARE (not reported to apps), OPTIONS (lit while pressed), PS button, speaker.
    for (int side = -1; side <= 1; side += 2) {
        const bool on = side > 0 && (p.buttons & PAD_BUTTON_OPTIONS);
        const Vec3 c = at(side * 0.0360f, face, 0.0092f);
        const Vec3 q[8] = {c - X * 0.0019f - Z * 0.0032f, c + X * 0.0019f - Z * 0.0032f, c + X * 0.0019f + Z * 0.0032f,
                           c - X * 0.0019f + Z * 0.0032f, c - X * 0.0019f - Z * 0.0032f + Y * 0.0015f,
                           c + X * 0.0019f - Z * 0.0032f + Y * 0.0015f, c + X * 0.0019f + Z * 0.0032f + Y * 0.0015f,
                           c - X * 0.0019f + Z * 0.0032f + Y * 0.0015f};
        box3d(t, q, on ? lit : dim);
    }
    prism3d(t, at(0, face, 0.0506f), X, Z, Y * 0.0012f, 0.0032f, 6, dim);

    // L1 / R1 on the front corners, L2 / R2 under them tilting in with the analog value.
    for (int side = -1; side <= 1; side += 2) {
        const bool l1 = (p.buttons & (side < 0 ? PAD_BUTTON_L1 : PAD_BUTTON_R1)) != 0;
        const float x0 = side * 0.0420f, x1 = side * 0.0660f;
        const Vec3 q[8] = {at(x0, 0.000f, -0.0040f), at(x1, 0.000f, -0.0040f), at(x1, 0.000f, 0.0030f),
                           at(x0, 0.000f, 0.0030f),  at(x0, 0.005f, -0.0040f), at(x1, 0.005f, -0.0040f),
                           at(x1, 0.005f, 0.0030f),  at(x0, 0.005f, 0.0030f)};
        box3d(t, q, l1 ? lit : dim);
        // Trigger: a box hinged at its top, swinging from sticking out (released) inwards.
        const float v = side < 0 ? p.l2 : p.r2;
        const float a = (25.0f + 50.0f * v) * 0.0174533f;
        const Vec3 hinge = at(side * 0.054f, -0.003f, 0.004f);
        const Vec3 down = Z * -cosf(a) + Y * -sinf(a); // from the hinge to the tip
        const Vec3 thick = Z * sinf(a) * 0.004f + Y * -cosf(a) * 0.004f;
        const Vec3 half = X * 0.0105f;
        const Vec3 tip = hinge + down * 0.020f;
        const Vec3 tq[8] = {hinge - half, hinge + half, tip + half, tip - half,
                            hinge - half + thick, hinge + half + thick, tip + half + thick, tip - half + thick};
        const uint32_t tc = v >= 0.9f ? gold : PIXEL_ALPHA | (v > 0.02f ? grey(0.45f + 0.55f * v) & 0xffffff : 0x505860);
        box3d(t, tq, tc);
    }

    // Rumble: waves spreading out of each grip, more and wider with the motor strength
    // (large motor in the left grip, small one in the right grip).
    for (int side = -1; side <= 1; side += 2) {
        const float m = side < 0 ? p.rumble_large : p.rumble_small;
        if (m <= 0.02f)
            continue;
        const Vec3 c = at(side * 0.064f, -0.020f, 0.094f);
        const float phase = time_s * (side < 0 ? 6.0f : 11.0f);
        const int waves = 1 + (int)(m * 2.99f);
        for (int k = 0; k < waves; k++) {
            float f = phase + k / (float)waves;
            f -= floorf(f);
            const float r = 0.014f + f * (0.010f + 0.012f * m);
            const uint32_t col = PIXEL_ALPHA | (grey((1.0f - f) * (0.4f + 0.6f * m)) & 0xffffff);
            const Vec3 u = X * (float)side, w = Z;
            Vec3 prev = c + (u * cosf(-1.2f) + w * sinf(-1.2f)) * r;
            for (int j = 1; j <= 8; j++) {
                const float ang = -1.2f + 2.4f * j / 8;
                Vec3 q = c + (u * cosf(ang) + w * sinf(ang)) * r;
                line3d(t, prev, q, col);
                prev = q;
            }
        }
    }

    // Battery where the speaker is, between the touchpad and the PS button.
    if (p.battery >= 0.0f)
        draw_battery(t, at(0, face, 0.0395f), X, Z, p.battery, p.charging);

    // Not tracked: said in front of it, on the plane of its top face.
    if (p.floating) {
        const char *txt = "Not tracked";
        const float h = 0.0085f;
        draw_text3d(t, at(-lobby_text_width(txt, h) * 0.5f, face, -0.012f), X, Z * -1.0f, h, txt,
                    PIXEL_ALPHA | 0xa0a8b0);
    }
}

static void draw_info_panel(const EyeTarget &t, const LobbyView *view)
{
    const float h = 0.12f, gap = 0.21f;
    Vec3 right = v3(cosf(view->info_yaw), 0, -sinf(view->info_yaw));
    Vec3 up = v3(0, 1, 0);
    int n = 0;
    while (n < 8 && view->info[n])
        n++;
    for (int i = 0; i < n; i++) {
        const float hi = i == 0 ? h * 1.4f : h; // title larger
        float w = lobby_text_width(view->info[i], hi);
        Vec3 o = view->info_pos + right * (-w * 0.5f) + up * ((n - 1) * gap * 0.5f - i * gap);
        draw_text3d(t, o, right, up, hi, view->info[i], PIXEL_ALPHA | 0xffffff);
    }
}

static void draw_panel(const EyeTarget &t, const LobbyPanel &p)
{
    auto at = [&](float px, float py) { return p.origin + p.right * px + p.up * py; };
    for (int i = 0; i < p.count; i++) {
        const LobbyPanelItem &it = p.items[i];
        if (it.fill)
            fill_quad3d(t, at(it.x0, it.y0), at(it.x1, it.y0), at(it.x1, it.y1), at(it.x0, it.y1), it.fill);
        if (it.outline) {
            const uint32_t c = PIXEL_ALPHA | it.outline;
            line3d(t, at(it.x0, it.y0), at(it.x1, it.y0), c);
            line3d(t, at(it.x1, it.y0), at(it.x1, it.y1), c);
            line3d(t, at(it.x1, it.y1), at(it.x0, it.y1), c);
            line3d(t, at(it.x0, it.y1), at(it.x0, it.y0), c);
        }
        if (it.text && it.text[0]) {
            const float w = lobby_text_width(it.text, it.text_h), margin = it.text_h * 0.6f;
            float tx = it.align < 0 ? it.x0 + margin : it.align > 0 ? it.x1 - margin - w : (it.x0 + it.x1 - w) * 0.5f;
            float ty = (it.y0 + it.y1 - it.text_h) * 0.5f;
            draw_text3d(t, at(tx, ty), p.right, p.up, it.text_h, it.text, PIXEL_ALPHA | it.text_rgb);
        }
    }
    for (int i = 0; i < p.shape_count; i++) {
        const LobbyPanelShape &s = p.shapes[i];
        if (s.r > 0.0f)
            circle3d(t, at(s.x0, s.y0), p.right, p.up, s.r, PIXEL_ALPHA | s.rgb);
        else
            line3d(t, at(s.x0, s.y0), at(s.x1, s.y1), PIXEL_ALPHA | s.rgb);
    }
}

static void draw_pointer(const EyeTarget &t, const LobbyPointer &ptr, const LobbyPanel *panel)
{
    line3d(t, ptr.from, ptr.to, PIXEL_ALPHA | ptr.rgb);
    if (ptr.hit && panel) {
        circle3d(t, ptr.to, panel->right, panel->up, 0.008f, PIXEL_ALPHA | 0xffffff);
        circle3d(t, ptr.to, panel->right, panel->up, 0.003f, PIXEL_ALPHA | 0xffffff);
    }
}

// Scales every pixel of the eye image by f (0..256), keeping the alpha.
static void darken(uint32_t *pixels, int width, int height, int pitch, uint32_t f)
{
    for (int yy = 0; yy < height; yy++) {
        uint32_t *row = pixels + (size_t)yy * pitch;
        for (int xx = 0; xx < width; xx++) {
            uint32_t p = row[xx];
            row[xx] = PIXEL_ALPHA | (((p & 0xff00ff) * f >> 8) & 0xff00ff) | (((p & 0x00ff00) * f >> 8) & 0x00ff00);
        }
    }
}

static void fill(uint32_t *pixels, int width, int height, int pitch, uint32_t c)
{
    const uint64_t c2 = (uint64_t)c << 32 | c;
    for (int yy = 0; yy < height; yy++) {
        uint64_t *row = (uint64_t *)(pixels + (size_t)yy * pitch);
        for (int xx = 0; xx < width / 2; xx++)
            row[xx] = c2;
    }
}

void lobby_render_eye(uint32_t *pixels, int width, int height, int pitch, const LobbyView *view, int eye)
{
    EyeTarget t;
    t.pixels = pixels;
    t.x0 = 0;
    t.width = width;
    t.height = height;
    t.pitch = pitch;
    t.eye_pos = view->eye_pos[eye];
    t.inv_rot = conj(view->eye_rot[eye]);
    t.fov = view->fov[eye];
    const float b = view->brightness;
    if (view->beacon) {
        fill(pixels, width, height, pitch, PIXEL_ALPHA);
        draw_camera(t, view->beacon_pos, view->beacon_rgb);
    } else {
        // The surroundings, faded with the brightness; then the camera and the controllers
        // at full brightness.
        if (b <= 0.004f) {
            fill(pixels, width, height, pitch, PIXEL_ALPHA);
        } else {
            fill(pixels, width, height, pitch, PIXEL_ALPHA | 0x06080c);
            if (view->grid_visible)
                draw_grid(t, view);
            if (view->info[0])
                draw_info_panel(t, view);
            if (view->panel && view->panel->visible)
                draw_panel(t, *view->panel);
            for (auto &ptr : view->pointers)
                if (ptr.visible)
                    draw_pointer(t, ptr, view->panel);
            if (b < 0.996f)
                darken(pixels, width, height, pitch, (uint32_t)(b * 256.0f));
        }
        draw_camera(t, v3(0, 0, 0), 0x40c0ff);
        for (auto &c : view->controllers)
            if (c.visible)
                draw_controller(t, c);
        for (auto &p : view->pads)
            if (p.visible)
                draw_pad(t, p, view->time_s);
    }
    // Text attached to the view, 2 m ahead of the head.
    if (view->overlay_text && view->overlay_brightness > 0.004f) {
        const float h = 0.075f, w = lobby_text_width(view->overlay_text, h);
        const Quat q = view->head_rot;
        Vec3 right = rotate(q, v3(1, 0, 0)), up = rotate(q, v3(0, 1, 0));
        Vec3 o = view->head_pos + rotate(q, v3(-w * 0.5f, -h * 0.5f, -2.0f));
        int v = (int)(view->overlay_brightness * 255.0f);
        v = v > 255 ? 255 : v;
        draw_text3d(t, o, right, up, h, view->overlay_text, PIXEL_ALPHA | v << 16 | v << 8 | v);
    }
}

void lobby_render(uint32_t *pixels, int width, int height, int pitch, const LobbyView *view)
{
    const int eye_w = width / 2;
    lobby_render_eye(pixels, eye_w, height, pitch, view, 0);
    lobby_render_eye(pixels + eye_w, eye_w, height, pitch, view, 1);
}
