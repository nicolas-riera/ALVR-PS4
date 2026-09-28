#include "reproj.h"

#include <string.h>

#include <orbis/libkernel.h>

#include "log.h"

// sceHmdReprojectionInitialize parameter (0x38 bytes). libSceHmd checks: both buffers
// non-null and different, cpu mask <= 0x3f, the two modes <= 6 / <= 7, rest zero.
struct ReprojInitParam {
    void *onion;
    void *garlic;
    uint64_t thread_priority;
    uint64_t cpu_mask;
    uint32_t mode_a;
    uint32_t mode_b;
    uint32_t reserved0;
    uint32_t reserved1;
    uint64_t reserved2;
};

// sceHmdReprojectionStart2dVr parameter (10 qwords). [0], [1] and [4] must be
// non-null, [6..9] must be zero. The 2D mesh builder (libSceHmd 0xf730) copies
// the T# from [0], a 16-byte sampler from [1] and the 16 bytes at [2..3].
struct Reproj2dParam {
    const GnmTexture *texture;
    const void *sampler;   // S#, 16 bytes
    float uv_transform[4]; // scale x, scale y, offset x, offset y (all 0 samples one texel)
    void *label;
    uint32_t time_us; // 2000..6999; Beat Saber uses 3000
    uint32_t pad;
    uint64_t zero[4];
};

// sceHmdReprojectionStart parameter (16 qwords). libSceHmd checks [0], [1], [2], [7]
// non-null, [7] 8-aligned, [8] in 2000..6999, [10] < 2, [0xb] & ~0xf0000000f == 0,
// [0xc..0xf] zero. The stereo mesh builder (0x94c0) copies T#s from [0] and [1],
// a sampler from [2], and per-eye 16-byte blocks from [3..4] and [5..6]: where the
// eye texture sits in tangent space, {half width, half height, centre x, centre y}.
struct ReprojStereoParam {
    const GnmTexture *left;
    const GnmTexture *right;
    const void *sampler;
    float fov_left[4];
    float fov_right[4];
    void *label;
    uint32_t time_us;
    uint32_t pad;
    uint64_t unk9;
    uint32_t mode; // < 2
    uint32_t pad2;
    uint64_t flags;
    uint64_t zero[4];
};

static_assert(sizeof(ReprojPose) == 0x38, "ReprojPose size");
static_assert(__builtin_offsetof(ReprojPose, orientation) == 0xc, "ReprojPose orientation");
static_assert(__builtin_offsetof(ReprojPose, timestamp) == 0x20, "ReprojPose timestamp");
static_assert(sizeof(ReprojStereoParam) == 0x80, "ReprojStereoParam size");
static_assert(sizeof(ReprojInitParam) == 0x38, "ReprojInitParam size");
static_assert(sizeof(Reproj2dParam) == 0x50, "Reproj2dParam size");

typedef uint64_t (*PFN_Query)();
typedef int (*PFN_Initialize)(const ReprojInitParam *, uint32_t type, void *reserved);
typedef int (*PFN_SetDisplayBuffers)(int32_t videoout, int32_t index_a, int32_t index_b, void *reserved);
typedef int (*PFN_Start2dVr)(const Reproj2dParam *, uint64_t frame, void *reserved);
typedef int (*PFN_Void)();
typedef int (*PFN_Start)(const ReprojStereoParam *, const ReprojPose *, uint64_t frame, void *reserved);

static PFN_Start p_start;

static PFN_Start2dVr p_start_2d;
static PFN_Void p_stop, p_finalize;
static int (*p_unset_display_buffers)();
static bool g_active;
static uint64_t g_frame;
alignas(64) static uint8_t g_label[1024];
alignas(16) static uint32_t g_sampler[4];

// AMD GCN sampler descriptor (S#): clamp to last texel, bilinear, no mips.
static void init_sampler()
{
    const uint32_t clamp_last_texel = 2, bilinear = 1;
    g_sampler[0] = clamp_last_texel | clamp_last_texel << 3 | clamp_last_texel << 6;
    g_sampler[1] = 0;                                  // min_lod = max_lod = 0
    g_sampler[2] = bilinear << 20 | bilinear << 22;    // xy mag / min filter
    g_sampler[3] = 0;
}

static void *resolve(int module, const char *name)
{
    void *fn = nullptr;
    if (sceKernelDlsym(module, name, &fn) != 0 || !fn) {
        LOG("reproj: symbol %s not found", name);
        return nullptr;
    }
    return fn;
}

static void *alloc_direct(size_t size, size_t align, int mem_type, const char *what)
{
    if (align < 0x4000)
        align = 0x4000;
    size = (size + align - 1) / align * align;
    off_t phys = 0;
    int rc = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), size, align, mem_type, &phys);
    if (rc < 0) {
        LOG("reproj: alloc %s (0x%zx) failed 0x%08x", what, size, (unsigned)rc);
        return nullptr;
    }
    void *ptr = nullptr;
    rc = sceKernelMapDirectMemory(&ptr, size, 0x33, 0, phys, align);
    if (rc < 0) {
        LOG("reproj: map %s failed 0x%08x", what, (unsigned)rc);
        return nullptr;
    }
    memset(ptr, 0, size);
    return ptr;
}

void gnm_texture_linear_bgra(GnmTexture *t, const void *base, uint32_t width, uint32_t height,
                             uint32_t pitch_pixels)
{
    // Field layout: shadPS4 src/video_core/amdgpu/resource.h (struct Image).
    const uint64_t addr = (uint64_t)base >> 8;
    const uint32_t data_format = 10; // 8_8_8_8
    const uint32_t num_format = 9;   // sRGB
    // Memory holds B,G,R,A: component x is blue, so R <- z, G <- y, B <- x; alpha forced to 1.
    const uint32_t sel_x = 6, sel_y = 5, sel_z = 4, sel_w = 1;
    const uint32_t tiling_index = 8; // DisplayLinearAligned
    const uint32_t type = 9;         // Color2D
    memset(t, 0, sizeof(*t));
    t->dw[0] = (uint32_t)addr;
    t->dw[1] = (uint32_t)(addr >> 32) & 0x3f;          // base_address[37:32]
    t->dw[1] |= data_format << 20 | num_format << 26;   // min_lod = 0
    t->dw[2] = (width - 1) | (height - 1) << 14;
    t->dw[3] = sel_x | sel_y << 3 | sel_z << 6 | sel_w << 9 | tiling_index << 20 | type << 28;
    t->dw[4] = (pitch_pixels - 1) << 13; // depth = 0
    t->dw[5] = 0;                        // base_array = last_array = 0
}

bool reproj_start(int module, int videoout, int first_index)
{
    if (module < 0 || videoout < 0)
        return false;
    auto q_onion = (PFN_Query)resolve(module, "sceHmdReprojectionQueryOnionBuffSize");
    auto q_onion_align = (PFN_Query)resolve(module, "sceHmdReprojectionQueryOnionBuffAlign");
    auto q_garlic = (PFN_Query)resolve(module, "sceHmdReprojectionQueryGarlicBuffSize");
    auto q_garlic_align = (PFN_Query)resolve(module, "sceHmdReprojectionQueryGarlicBuffAlign");
    auto initialize = (PFN_Initialize)resolve(module, "sceHmdReprojectionInitialize");
    auto set_display_buffers = (PFN_SetDisplayBuffers)resolve(module, "sceHmdReprojectionSetDisplayBuffers");
    p_start_2d = (PFN_Start2dVr)resolve(module, "sceHmdReprojectionStart2dVr");
    p_start = (PFN_Start)resolve(module, "sceHmdReprojectionStart");
    p_stop = (PFN_Void)resolve(module, "sceHmdReprojectionStop");
    p_finalize = (PFN_Void)resolve(module, "sceHmdReprojectionFinalize");
    p_unset_display_buffers = (int (*)())resolve(module, "sceHmdReprojectionUnsetDisplayBuffers");
    if (!q_onion || !q_onion_align || !q_garlic || !q_garlic_align || !initialize || !set_display_buffers ||
        !p_start_2d)
        return false;

    uint64_t onion_size = q_onion(), onion_align = q_onion_align();
    uint64_t garlic_size = q_garlic(), garlic_align = q_garlic_align();
    LOG("reproj buffers: onion 0x%llx/0x%llx garlic 0x%llx/0x%llx", (unsigned long long)onion_size,
        (unsigned long long)onion_align, (unsigned long long)garlic_size, (unsigned long long)garlic_align);
    void *onion = alloc_direct(onion_size, onion_align, 0 /* WB onion */, "onion");
    void *garlic = alloc_direct(garlic_size, garlic_align, 3 /* WC garlic */, "garlic");
    if (!onion || !garlic)
        return false;

    ReprojInitParam ip;
    memset(&ip, 0, sizeof(ip));
    ip.onion = onion;
    ip.garlic = garlic;
    ip.thread_priority = 0x100;
    ip.cpu_mask = 0x38;
    ip.mode_a = 5;
    ip.mode_b = 5;
    // The type argument (0..2) comes from a Unity setting we could not pin down; try each.
    int rc = -1;
    static const uint32_t types[] = {1, 2, 0};
    for (uint32_t type : types) {
        rc = initialize(&ip, type, nullptr);
        LOG("sceHmdReprojectionInitialize(type %u) -> 0x%08x", type, (unsigned)rc);
        if (rc == 0)
            break;
    }
    if (rc != 0)
        return false;

    rc = set_display_buffers(videoout, first_index, first_index + 1, nullptr);
    LOG("sceHmdReprojectionSetDisplayBuffers(%d, %d, %d) -> 0x%08x", videoout, first_index, first_index + 1,
        (unsigned)rc);
    if (rc != 0)
        return false;
    init_sampler();
    g_active = true;
    return true;
}

int reproj_submit_2d(const GnmTexture *tex)
{
    if (!g_active)
        return -1;
    Reproj2dParam p;
    memset(&p, 0, sizeof(p));
    p.texture = tex;
    p.sampler = g_sampler;
    p.label = g_label;
    p.uv_transform[0] = 1.0f;
    p.uv_transform[1] = 1.0f;
    p.uv_transform[2] = 0.0f;
    p.uv_transform[3] = 0.0f;
    p.time_us = 3000;
    static int last_rc = 1;
    int rc = p_start_2d(&p, g_frame++, nullptr);
    if (rc != last_rc) {
        LOG("sceHmdReprojectionStart2dVr -> 0x%08x (frame %llu)", (unsigned)rc, (unsigned long long)g_frame);
        last_rc = rc;
    }
    return rc;
}

int reproj_submit_stereo(const GnmTexture *left, const GnmTexture *right, const float fov_left[4],
                         const float fov_right[4], const ReprojPose *pose)
{
    if (!g_active || !p_start)
        return -1;
    ReprojStereoParam p;
    memset(&p, 0, sizeof(p));
    p.left = left;
    p.right = right;
    p.sampler = g_sampler;
    memcpy(p.fov_left, fov_left, sizeof(p.fov_left));
    memcpy(p.fov_right, fov_right, sizeof(p.fov_right));
    p.label = g_label;
    p.time_us = 3000;
    static int last_rc = 1;
    int rc = p_start(&p, pose, g_frame++, nullptr);
    if (rc != last_rc) {
        LOG("sceHmdReprojectionStart -> 0x%08x (frame %llu)", (unsigned)rc, (unsigned long long)g_frame);
        last_rc = rc;
    }
    return rc;
}

void reproj_stop()
{
    if (!g_active)
        return;
    if (p_stop)
        LOG("sceHmdReprojectionStop -> 0x%08x", (unsigned)p_stop());
    if (p_unset_display_buffers)
        LOG("sceHmdReprojectionUnsetDisplayBuffers -> 0x%08x", (unsigned)p_unset_display_buffers());
    if (p_finalize)
        LOG("sceHmdReprojectionFinalize -> 0x%08x", (unsigned)p_finalize());
    g_active = false;
}

bool reproj_active()
{
    return g_active;
}
