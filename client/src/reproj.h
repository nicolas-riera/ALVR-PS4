#pragma once

#include <stdint.h>

// System VR compositor (libSceHmd reprojection). Displaying through it is what puts
// the PS4 in its VR mode (PSVR quick menu, lens distortion, 120 Hz) and lets the
// system restore everything when the app is closed from the PS4 menu.
// Call sequence follows Beat Saber (reference/decomp/beatsaber_psvr.c) and the
// checks in libSceHmd (reference/decomp/hmd_reprojection.c).

// AMD GCN texture descriptor (T#), 8 dwords.
struct GnmTexture {
    uint32_t dw[8];
};

// Describes a linear 32-bit BGRA (VideoOut A8R8G8B8) image as an sRGB 2D texture.
void gnm_texture_linear_bgra(GnmTexture *t, const void *base, uint32_t width, uint32_t height,
                             uint32_t pitch_pixels);

// Initializes reprojection and hands it the VideoOut buffers [first_index, first_index+1]
// (1920x1080, tiled, registered by screen_register_vr_buffers).
bool reproj_start(int hmd_module, int videoout_handle, int first_index);
// Shows a 2D image on the floating screen of the system VR mode.
int reproj_submit_2d(const GnmTexture *tex);
// Stops reprojection and releases the display buffers.
void reproj_stop();
bool reproj_active();
