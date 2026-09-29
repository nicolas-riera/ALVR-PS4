#pragma once

#include <stdint.h>

// Minimal double-buffered 2D framebuffer on the TV output (sceVideoOut).

struct Screen {
    int width;
    int height;
    int handle;
    int cur;
    uint32_t *buffers[2];
    void *flip_queue;
};

bool screen_init(Screen *s, int width, int height);
void screen_fill(Screen *s, uint32_t rgb);
void screen_rect(Screen *s, int x, int y, int w, int h, uint32_t rgb);
// Draws ASCII text at scale 1 (12x24 cells). Returns the x after the last glyph.
int screen_text(Screen *s, int x, int y, const char *txt, uint32_t rgb);
void screen_flip(Screen *s);
// Registers two extra tiled 1920x1080 buffers, used as reprojection output.
bool screen_register_vr_buffers(Screen *s, int first_index);
// Switches the video output to the PSVR mode, as VR games do: *hz is 90 (89.91 Hz) or
// 120 (119.88 Hz) on input; on return, the rate in use (90 falls back to 120).
bool screen_set_vr_output_mode(Screen *s, int videoout_module, int *hz);
