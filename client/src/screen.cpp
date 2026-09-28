#include "screen.h"

#include <string.h>

#include <orbis/libkernel.h>
#include <orbis/VideoOut.h>

#include "font_data.h"
#include "log.h"

static const uint32_t PIXEL_ALPHA = 0x80000000; // format used by the toolchain samples

static bool screen_init_impl(Screen *s, int width, int height);

bool screen_init(Screen *s, int width, int height)
{
    bool ok = screen_init_impl(s, width, height);
    if (!ok)
        s->handle = -1;
    return ok;
}

static bool screen_init_impl(Screen *s, int width, int height)
{
    memset(s, 0, sizeof(*s));
    s->width = width;
    s->height = height;

    s->handle = sceVideoOutOpen(ORBIS_VIDEO_USER_MAIN, ORBIS_VIDEO_OUT_BUS_MAIN, 0, 0);
    if (s->handle < 0) {
        LOG("sceVideoOutOpen failed: 0x%08x", s->handle);
        return false;
    }

    OrbisKernelEqueue *q = new OrbisKernelEqueue;
    int rc = sceKernelCreateEqueue(q, "alvr flip queue");
    if (rc < 0) {
        LOG("sceKernelCreateEqueue failed: 0x%08x", rc);
        return false;
    }
    sceVideoOutAddFlipEvent(*q, s->handle, 0);
    s->flip_queue = q;

    const size_t align = 0x200000;
    size_t fb_size = (size_t)width * height * 4;
    size_t total = (fb_size * 2 + align - 1) / align * align;

    off_t phys = 0;
    rc = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), total, align, 3, &phys);
    if (rc < 0) {
        LOG("sceKernelAllocateDirectMemory failed: 0x%08x", rc);
        return false;
    }
    void *mem = nullptr;
    rc = sceKernelMapDirectMemory(&mem, total, 0x33, 0, phys, align);
    if (rc < 0) {
        LOG("sceKernelMapDirectMemory failed: 0x%08x", rc);
        return false;
    }

    s->buffers[0] = (uint32_t *)mem;
    s->buffers[1] = (uint32_t *)((char *)mem + fb_size);

    OrbisVideoOutBufferAttribute attr;
    sceVideoOutSetBufferAttribute(&attr, 0x80000000, 1, 0, width, height, width);
    rc = sceVideoOutRegisterBuffers(s->handle, 0, (void **)s->buffers, 2, &attr);
    if (rc != 0) {
        LOG("sceVideoOutRegisterBuffers failed: 0x%08x", rc);
        return false;
    }
    sceVideoOutSetFlipRate(s->handle, 0);
    return true;
}

void screen_fill(Screen *s, uint32_t rgb)
{
    uint32_t *fb = s->buffers[s->cur];
    uint32_t px = PIXEL_ALPHA | rgb;
    size_t n = (size_t)s->width * s->height;
    for (size_t i = 0; i < n; i++)
        fb[i] = px;
}

void screen_rect(Screen *s, int x, int y, int w, int h, uint32_t rgb)
{
    uint32_t *fb = s->buffers[s->cur];
    uint32_t px = PIXEL_ALPHA | rgb;
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > s->width ? s->width : x + w;
    int y1 = y + h > s->height ? s->height : y + h;
    for (int yy = y0; yy < y1; yy++) {
        uint32_t *row = fb + (size_t)yy * s->width;
        for (int xx = x0; xx < x1; xx++)
            row[xx] = px;
    }
}

int screen_text(Screen *s, int x, int y, const char *txt, uint32_t rgb)
{
    uint32_t *fb = s->buffers[s->cur];
    uint32_t px = PIXEL_ALPHA | rgb;
    for (const char *p = txt; *p; p++, x += FONT_W) {
        unsigned char c = (unsigned char)*p;
        if (c < FONT_FIRST || c > FONT_LAST)
            c = '?';
        if (x + FONT_W > s->width)
            break;
        const uint16_t *glyph = font_data[c - FONT_FIRST];
        for (int gy = 0; gy < FONT_H; gy++) {
            int yy = y + gy;
            if (yy < 0 || yy >= s->height)
                continue;
            uint16_t bits = glyph[gy];
            uint32_t *row = fb + (size_t)yy * s->width + x;
            for (int gx = 0; gx < FONT_W; gx++)
                if (bits & (1 << (FONT_W - 1 - gx)))
                    row[gx] = px;
        }
    }
    return x;
}

void screen_flip(Screen *s)
{
    static int64_t flip_arg = 0;
    flip_arg++;
    sceVideoOutSubmitFlip(s->handle, s->cur, ORBIS_VIDEO_OUT_FLIP_VSYNC, flip_arg);

    OrbisKernelEqueue q = *(OrbisKernelEqueue *)s->flip_queue;
    for (;;) {
        OrbisVideoOutFlipStatus st;
        sceVideoOutGetFlipStatus(s->handle, &st);
        if (st.flipArg >= flip_arg)
            break;
        OrbisKernelEvent ev;
        int count;
        if (sceKernelWaitEqueue(q, &ev, 1, &count, 0) != 0)
            break;
    }
    s->cur ^= 1;
}

bool screen_register_vr_buffers(Screen *s, int first_index)
{
    // Output buffers for the reprojection: 1920x1080, tiled, option 7 (the combination
    // sceHmdReprojectionSetDisplayBuffers accepts). Only the GPU writes them.
    const size_t align = 0x200000;
    const size_t each = 0x1000000; // generous for tiled padding
    off_t phys = 0;
    int rc = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), each * 2, align, 3, &phys);
    if (rc < 0) {
        LOG("vr buffers: alloc failed 0x%08x", (unsigned)rc);
        return false;
    }
    void *mem = nullptr;
    rc = sceKernelMapDirectMemory(&mem, each * 2, 0x33, 0, phys, align);
    if (rc < 0) {
        LOG("vr buffers: map failed 0x%08x", (unsigned)rc);
        return false;
    }
    void *bufs[2] = {mem, (char *)mem + each};
    OrbisVideoOutBufferAttribute attr;
    memset(&attr, 0, sizeof(attr));
    attr.format = (int32_t)0x80000000; // A8R8G8B8 sRGB
    attr.tmode = 0;                    // tiled
    attr.aspect = 0;
    attr.width = 1920;
    attr.height = 1080;
    attr.pixelPitch = 1920;
    attr.reserved[0] = 7; // SCE_VIDEO_OUT_BUFFER_ATTRIBUTE_OPTION_VR
    // Returns the buffer group index (>= 0) on success.
    rc = sceVideoOutRegisterBuffers(s->handle, first_index, bufs, 2, &attr);
    LOG("vr buffers: sceVideoOutRegisterBuffers(index %d) -> 0x%08x", first_index, (unsigned)rc);
    return rc >= 0;
}

// sceVideoOutConfigureOutputMode_ mode (shadPS4 videoout: struct Mode).
struct VideoOutMode {
    uint32_t size;
    uint8_t signal_encoding, signal_range, colorimetry, depth;
    uint64_t refresh_rate;
    uint64_t resolution;
    uint8_t reserved[8];
};
static_assert(sizeof(VideoOutMode) == 0x20, "VideoOutMode size");

typedef int (*PFN_ConfigureOutputMode)(int32_t handle, uint32_t reserved, const VideoOutMode *mode,
                                       const void *options, uint32_t mode_size, uint32_t options_size);

bool screen_set_vr_output_mode(Screen *s, int videoout_module)
{
    // What Unity does for PSVR: "any" mode (all 0xff), 119.88 Hz, and this resolution mask.
    // This is the output mode switch that makes capture cards drop and re-sync.
    PFN_ConfigureOutputMode configure = nullptr;
    if (sceKernelDlsym(videoout_module, "sceVideoOutConfigureOutputMode_", (void **)&configure) != 0 || !configure) {
        LOG("sceVideoOutConfigureOutputMode_ not found");
        return false;
    }
    VideoOutMode mode;
    memset(&mode, 0xff, sizeof(mode));
    mode.size = sizeof(mode);
    mode.refresh_rate = 13; // SCE_VIDEO_OUT_REFRESH_RATE_119_88HZ
    mode.resolution = 0xFFFFFFFFC1FFFFFFull;
    int rc = configure(s->handle, 0, &mode, nullptr, sizeof(mode), 0x10);
    LOG("sceVideoOutConfigureOutputMode_(119.88 Hz) -> 0x%08x", (unsigned)rc);
    if (rc < 0) {
        uint8_t options[0x10];
        memset(options, 0, sizeof(options));
        rc = configure(s->handle, 0, &mode, options, sizeof(mode), sizeof(options));
        LOG("sceVideoOutConfigureOutputMode_(119.88 Hz, zeroed options) -> 0x%08x", (unsigned)rc);
    }
    return rc >= 0;
}
