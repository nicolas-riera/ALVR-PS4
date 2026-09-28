#pragma once

// Minimal 3D math for the software renderer.

#include <math.h>

struct Vec3 {
    float x, y, z;
};

struct Quat {
    float x, y, z, w;
};

static inline Vec3 v3(float x, float y, float z) { return Vec3{x, y, z}; }
static inline Vec3 operator+(Vec3 a, Vec3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline Vec3 operator-(Vec3 a, Vec3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline Vec3 operator*(Vec3 a, float s) { return v3(a.x * s, a.y * s, a.z * s); }
static inline Vec3 cross(Vec3 a, Vec3 b) { return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }

static inline Quat conj(Quat q) { return Quat{-q.x, -q.y, -q.z, q.w}; }

// Rotates v by the unit quaternion q.
static inline Vec3 rotate(Quat q, Vec3 v)
{
    Vec3 u = v3(q.x, q.y, q.z);
    Vec3 t = cross(u, v) * 2.0f;
    return v + t * q.w + cross(u, t);
}
