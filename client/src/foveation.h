#pragma once

#include <stdint.h>

// ALVR 20.14.1 foveated encoding. The streamer keeps the center of each eye at full
// resolution and squeezes the edges by up to edge_ratio before encoding, so the decoder
// gets far fewer pixels (the PSVR lenses blur the edges anyway). The client expands the
// picture back. Each axis is independent. Same math as the official client
// (alvr/graphics/src/stream.rs and resources/stream.wgsl) and the streamer
// (server_openvr/cpp/platform/win32/FFR.cpp, CompressAxisAlignedPixelShader.hlsl).

struct FoveationSettings {
    float center_size_x, center_size_y;
    float center_shift_x, center_shift_y;
    float edge_ratio_x, edge_ratio_y;
};

struct FoveationAxis {
    uint32_t expanded;   // pixels of one eye after expansion (negotiated view resolution)
    uint32_t compressed; // pixels of one eye in the decoded frame (multiple of 32)
    bool identity;       // no squeezing on this axis
    double ratio;        // unaligned compressed size / compressed
    double e, c1, c2, lo, hi, a_l, b_l, a_r, b_r, c_r;
};

void foveation_axis(FoveationAxis *a, uint32_t expanded, float center_size, float center_shift, float edge_ratio);

// Normalized expanded coordinate (0..1, left eye orientation) -> normalized coordinate in
// the compressed eye (0..1 over `compressed` pixels).
double foveation_map(const FoveationAxis *a, double u);
