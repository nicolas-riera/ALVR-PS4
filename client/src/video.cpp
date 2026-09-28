#include "video.h"

#include <pthread.h>
#include <string.h>

#include <orbis/libkernel.h>

#include "log.h"

// ---------------------------------------------------------------------------------------
// libSceVideodec2 structures (sizes are checked by the library: this_size fields)

struct Vdec2ConfigInfo {
    uint64_t this_size;              // 0x00 = 0x48
    uint32_t resource_type;          // 0x08 1 = decode + compute-queue output (0xb6c8/0x12384: system)
    uint32_t codec_type;             // 0x0c 1 = AVC, 0xee049 = HEVC
    uint32_t profile;                // 0x10 AVC: 66, 77 or 100
    uint32_t max_level;              // 0x14 AVC: 10..13, 20..22, 30..32, 40..42, 50..52, 60..62
    int32_t max_frame_width;         // 0x18
    int32_t max_frame_height;        // 0x1c
    int32_t max_dpb_frame_count;     // 0x20 -1..16
    uint32_t decode_pipeline_depth;  // 0x24 1..8; 1 = every Decode returns its own picture
    void *compute_queue;             // 0x28 required for resource_type 1
    uint64_t cpu_affinity_mask;      // 0x30 bits 0..5 (0..6 in some CPU modes)
    int32_t cpu_thread_priority;     // 0x38 -1 or 256..767
    uint8_t optimize_progressive;    // 0x3c
    uint8_t check_memory_type;       // 0x3d 1: memory types are verified (onion/garlic)
    uint8_t reserved0, reserved1;    // 0x3e must be 0
    void *extra_config_info;         // 0x40 must be NULL for AVC
};
static_assert(sizeof(Vdec2ConfigInfo) == 0x48, "config");

struct Vdec2MemoryInfo {
    uint64_t this_size; // 0x48
    uint64_t cpu_memory_size;
    void *cpu_memory;
    uint64_t gpu_memory_size;
    void *gpu_memory;
    uint64_t cpu_gpu_memory_size;
    void *cpu_gpu_memory;
    uint64_t max_frame_buffer_size;
    uint32_t frame_buffer_alignment;
    uint32_t reserved0; // must be 0
};
static_assert(sizeof(Vdec2MemoryInfo) == 0x48, "memory");

struct Vdec2InputData {
    uint64_t this_size; // 0x30
    const void *au_data;
    uint64_t au_size;
    uint64_t pts, dts, attached;
};
static_assert(sizeof(Vdec2InputData) == 0x30, "input");

struct Vdec2FrameBuffer {
    uint64_t this_size; // 0x20
    void *frame_buffer; // 256-byte aligned
    uint64_t frame_buffer_size;
    uint8_t is_accepted;
    uint8_t pad[7];
};
static_assert(sizeof(Vdec2FrameBuffer) == 0x20, "frame buffer");

struct Vdec2OutputInfo {
    uint64_t this_size; // 0x38
    uint8_t is_valid;
    uint8_t is_error_frame;
    uint8_t picture_count;
    uint8_t unk0b;
    uint32_t codec_type;
    uint32_t frame_width;
    uint32_t frame_pitch;
    uint32_t frame_height;
    uint32_t pad1c;
    void *frame_buffer;
    uint64_t frame_buffer_size;
    uint32_t frame_format; // 0, or 0xc24a
    uint32_t frame_pitch_in_bytes;
};
static_assert(sizeof(Vdec2OutputInfo) == 0x38, "output");

struct Vdec2ComputeMemoryInfo {
    uint64_t this_size; // 0x18
    uint64_t cpu_gpu_memory_size;
    void *cpu_gpu_memory;
};
struct Vdec2ComputeConfigInfo {
    uint64_t this_size; // 0x10
    uint16_t compute_pipe_id;  // 0..4
    uint16_t compute_queue_id; // 0..7
    uint8_t check_memory_type;
    uint8_t reserved0;
    uint16_t reserved1;
};
static_assert(sizeof(Vdec2ComputeConfigInfo) == 0x10, "compute config");

typedef int (*QueryComputeMemoryInfoFn)(Vdec2ComputeMemoryInfo *);
typedef int (*AllocateComputeQueueFn)(const Vdec2ComputeConfigInfo *, const Vdec2ComputeMemoryInfo *, void **);
typedef int (*QueryDecoderMemoryInfoFn)(const Vdec2ConfigInfo *, Vdec2MemoryInfo *);
typedef int (*CreateDecoderFn)(const Vdec2ConfigInfo *, const Vdec2MemoryInfo *, void **);
typedef int (*DeleteDecoderFn)(void *);
typedef int (*DecodeFn)(void *, const Vdec2InputData *, Vdec2FrameBuffer *, Vdec2OutputInfo *);
typedef int (*ResetFn)(void *);

static QueryComputeMemoryInfoFn p_query_compute;
static AllocateComputeQueueFn p_alloc_compute;
static QueryDecoderMemoryInfoFn p_query_decoder;
static CreateDecoderFn p_create;
static DeleteDecoderFn p_delete;
static DecodeFn p_decode;
static ResetFn p_reset;

// ---------------------------------------------------------------------------------------
// State

static const int QUEUE_SLOTS = 8;
static const size_t CONFIG_MAX = 1024;       // SPS/PPS, prepended to IDR frames in place
static const size_t SLOT_SIZE = 3 << 20;     // largest access unit accepted (+ CONFIG_MAX)
static const int EYE_SETS = 4;               // published, displayed, previously displayed, writing

struct Slot {
    uint8_t *mem; // CONFIG_MAX bytes of headroom, then the frame
    size_t len;
    uint64_t timestamp_ns;
    uint64_t received_us;
    bool idr;
};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;
static Slot g_slots[QUEUE_SLOTS];
static int g_head, g_count;            // queue of complete frames (net thread -> decoder)
static bool g_wait_idr = true;         // drop frames until an IDR arrives
static bool g_want_idr = false;
static uint64_t g_last_idr_request_us;
static uint32_t g_codec = 0xffffffff;
static uint8_t g_config[CONFIG_MAX];
static size_t g_config_len;
static unsigned g_config_gen;          // bumped by every DecoderConfig
static uint32_t g_view_w = 960, g_view_h = 1056;
static bool g_full_range = true;
static VideoStats g_stats;

// Decoder (decode thread only).
static void *g_compute_queue;
static void *g_decoder;
static unsigned g_decoder_gen;
static uint32_t g_dec_w, g_dec_h;
static void *g_frame_buffers[2];
static uint64_t g_frame_buffer_size;
static int g_fb_cur;

// Eye buffers.
static uint32_t *g_eye_mem[EYE_SETS][2];
static GnmTexture g_eye_tex[EYE_SETS][2];
static uint32_t g_eye_w, g_eye_h, g_eye_pitch;
static pthread_mutex_t g_pub_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_published = -1, g_displayed = -1, g_prev_displayed = -1;
static uint64_t g_pub_ts, g_pub_decoded_us;
static unsigned g_pub_seq;

static uint64_t now_us() { return sceKernelGetProcessTime(); }

static void *alloc_direct(size_t size, size_t align, int mem_type, const char *what)
{
    if (align < 0x10000)
        align = 0x10000;
    size = (size + align - 1) / align * align;
    off_t phys = 0;
    int rc = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), size, align, mem_type, &phys);
    if (rc < 0) {
        LOG("video: alloc %s (0x%zx) failed 0x%08x", what, size, (unsigned)rc);
        return nullptr;
    }
    void *ptr = nullptr;
    rc = sceKernelMapDirectMemory(&ptr, size, 0x33, 0, phys, align);
    if (rc < 0) {
        LOG("video: map %s failed 0x%08x", what, (unsigned)rc);
        return nullptr;
    }
    return ptr;
}

static const int MEM_ONION = 0, MEM_GARLIC = 3; // SCE_KERNEL_WB_ONION / SCE_KERNEL_WC_GARLIC

// ---------------------------------------------------------------------------------------
// NV12 -> BGRA (BT.709), one eye = one half of the side-by-side frame

struct ConvertJob {
    const uint8_t *y, *uv;
    uint32_t pitch, x0, w, h;
    uint32_t *dst;
    uint32_t dst_pitch;
    bool full_range;
};

static inline uint32_t clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : (uint32_t)v; }

static void convert(const ConvertJob &j)
{
    // Fixed point, 16 fractional bits.
    int ys, yo, rv, gu, gv, bu;
    if (j.full_range) {
        ys = 65536; yo = 0; rv = 103206; gu = 12276; gv = 30679; bu = 121609;
    } else {
        ys = 76309; yo = 16; rv = 117506; gu = 13959; gv = 34931; bu = 138412;
    }
    const uint32_t alpha = 0x80000000; // same as the lobby
    for (uint32_t row = 0; row < j.h; row += 2) {
        const uint8_t *y0 = j.y + (size_t)row * j.pitch + j.x0;
        const uint8_t *y1 = row + 1 < j.h ? y0 + j.pitch : y0;
        const uint8_t *uv = j.uv + (size_t)(row / 2) * j.pitch + j.x0;
        uint32_t *d0 = j.dst + (size_t)row * j.dst_pitch;
        uint32_t *d1 = row + 1 < j.h ? d0 + j.dst_pitch : d0;
        for (uint32_t x = 0; x < j.w; x += 2) {
            int u = uv[x] - 128, v = uv[x + 1] - 128;
            int cr = rv * v + 32768, cg = -gu * u - gv * v + 32768, cb = bu * u + 32768;
            int l;
            l = (y0[x] - yo) * ys;
            d0[x] = alpha | clamp8((l + cr) >> 16) << 16 | clamp8((l + cg) >> 16) << 8 | clamp8((l + cb) >> 16);
            l = (y0[x + 1] - yo) * ys;
            d0[x + 1] = alpha | clamp8((l + cr) >> 16) << 16 | clamp8((l + cg) >> 16) << 8 | clamp8((l + cb) >> 16);
            l = (y1[x] - yo) * ys;
            d1[x] = alpha | clamp8((l + cr) >> 16) << 16 | clamp8((l + cg) >> 16) << 8 | clamp8((l + cb) >> 16);
            l = (y1[x + 1] - yo) * ys;
            d1[x + 1] = alpha | clamp8((l + cr) >> 16) << 16 | clamp8((l + cg) >> 16) << 8 | clamp8((l + cb) >> 16);
        }
    }
}

// Helper thread converting the right eye while the decode thread does the left one.
static pthread_mutex_t g_job_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_job_cond = PTHREAD_COND_INITIALIZER;
static ConvertJob g_job;
static unsigned g_job_gen, g_job_done;

static void *convert_thread(void *)
{
    unsigned seen = 0;
    for (;;) {
        pthread_mutex_lock(&g_job_lock);
        while (g_job_gen == seen)
            pthread_cond_wait(&g_job_cond, &g_job_lock);
        seen = g_job_gen;
        ConvertJob j = g_job;
        pthread_mutex_unlock(&g_job_lock);
        convert(j);
        pthread_mutex_lock(&g_job_lock);
        g_job_done = seen;
        pthread_cond_broadcast(&g_job_cond);
        pthread_mutex_unlock(&g_job_lock);
    }
    return nullptr;
}

static bool alloc_eye_buffers(uint32_t w, uint32_t h)
{
    if (g_eye_mem[0][0] && w == g_eye_w && h == g_eye_h)
        return true;
    if (g_eye_mem[0][0]) {
        LOG("video: eye size changed to %ux%u, not supported yet", w, h);
        return false;
    }
    uint32_t pitch = (w + 63) & ~63u;
    size_t each = (size_t)pitch * h * 4;
    each = (each + 0xffff) & ~(size_t)0xffff;
    uint8_t *mem = (uint8_t *)alloc_direct(each * EYE_SETS * 2, 0x10000, MEM_ONION, "eye buffers");
    if (!mem)
        return false;
    memset(mem, 0, each * EYE_SETS * 2);
    for (int s = 0; s < EYE_SETS; s++)
        for (int e = 0; e < 2; e++) {
            g_eye_mem[s][e] = (uint32_t *)(mem + each * (s * 2 + e));
            gnm_texture_linear_bgra(&g_eye_tex[s][e], g_eye_mem[s][e], w, h, pitch);
        }
    g_eye_w = w;
    g_eye_h = h;
    g_eye_pitch = pitch;
    LOG("video: eye buffers %ux%u (pitch %u) x%d", w, h, pitch, EYE_SETS);
    return true;
}

// ---------------------------------------------------------------------------------------
// Decoder

static bool create_compute_queue()
{
    if (g_compute_queue)
        return true;
    Vdec2ComputeMemoryInfo mem{};
    mem.this_size = sizeof(mem);
    int rc = p_query_compute(&mem);
    if (rc != 0) {
        LOG("video: sceVideodec2QueryComputeMemoryInfo -> 0x%08x", (unsigned)rc);
        return false;
    }
    mem.cpu_gpu_memory = alloc_direct(mem.cpu_gpu_memory_size, 0x10000, MEM_ONION, "compute queue");
    if (!mem.cpu_gpu_memory)
        return false;
    // Pipes/queues possibly taken by the tracker or the compositor are skipped on failure.
    static const uint16_t tries[][2] = {{2, 0}, {3, 0}, {1, 0}, {2, 3}, {3, 5}, {0, 6}};
    for (auto &t : tries) {
        Vdec2ComputeConfigInfo cfg{};
        cfg.this_size = sizeof(cfg);
        cfg.compute_pipe_id = t[0];
        cfg.compute_queue_id = t[1];
        rc = p_alloc_compute(&cfg, &mem, &g_compute_queue);
        LOG("video: compute queue pipe %u queue %u (0x%zx bytes) -> 0x%08x", t[0], t[1],
            (size_t)mem.cpu_gpu_memory_size, (unsigned)rc);
        if (rc == 0)
            return true;
    }
    g_compute_queue = nullptr;
    return false;
}

static bool create_decoder(uint32_t width, uint32_t height)
{
    if (g_decoder) {
        p_delete(g_decoder);
        g_decoder = nullptr;
    }
    if (!create_compute_queue())
        return false;
    Vdec2ConfigInfo cfg{};
    cfg.this_size = sizeof(cfg);
    cfg.resource_type = 1;
    cfg.codec_type = 1; // AVC
    cfg.profile = 100;  // High (covers Main and Baseline streams)
    cfg.max_level = 52;
    cfg.max_frame_width = (int32_t)((width + 15) & ~15u);
    cfg.max_frame_height = (int32_t)((height + 15) & ~15u);
    cfg.max_dpb_frame_count = 16;
    cfg.decode_pipeline_depth = 1;
    cfg.compute_queue = g_compute_queue;
    cfg.cpu_affinity_mask = 0x3f;
    cfg.cpu_thread_priority = -1;
    cfg.optimize_progressive = 1;
    cfg.check_memory_type = 0;
    Vdec2MemoryInfo mem{};
    mem.this_size = sizeof(mem);
    int rc = p_query_decoder(&cfg, &mem);
    LOG("video: QueryDecoderMemoryInfo %dx%d -> 0x%08x cpu 0x%llx gpu 0x%llx cpu_gpu 0x%llx frame 0x%llx align 0x%x",
        cfg.max_frame_width, cfg.max_frame_height, (unsigned)rc, (unsigned long long)mem.cpu_memory_size,
        (unsigned long long)mem.gpu_memory_size, (unsigned long long)mem.cpu_gpu_memory_size,
        (unsigned long long)mem.max_frame_buffer_size, mem.frame_buffer_alignment);
    if (rc != 0)
        return false;
    // The decoder memory is allocated once for the largest size seen; a smaller stream
    // reuses it.
    static void *cpu_mem, *gpu_mem, *cpu_gpu_mem;
    static uint64_t cpu_size, gpu_size, cpu_gpu_size;
    if (mem.cpu_memory_size > cpu_size) {
        cpu_mem = alloc_direct(mem.cpu_memory_size, 0x10000, MEM_ONION, "decoder cpu");
        cpu_size = cpu_mem ? mem.cpu_memory_size : 0;
    }
    if (mem.gpu_memory_size > gpu_size) {
        gpu_mem = alloc_direct(mem.gpu_memory_size, 0x200000, MEM_GARLIC, "decoder gpu");
        gpu_size = gpu_mem ? mem.gpu_memory_size : 0;
    }
    if (mem.cpu_gpu_memory_size > cpu_gpu_size) {
        cpu_gpu_mem = alloc_direct(mem.cpu_gpu_memory_size, 0x10000, MEM_ONION, "decoder cpu_gpu");
        cpu_gpu_size = cpu_gpu_mem ? mem.cpu_gpu_memory_size : 0;
    }
    if (mem.max_frame_buffer_size > g_frame_buffer_size) {
        for (int i = 0; i < 2; i++)
            g_frame_buffers[i] = alloc_direct(mem.max_frame_buffer_size, 0x10000, MEM_ONION, "frame buffer");
        g_frame_buffer_size = g_frame_buffers[0] && g_frame_buffers[1] ? mem.max_frame_buffer_size : 0;
    }
    if ((mem.cpu_memory_size && !cpu_mem) || (mem.gpu_memory_size && !gpu_mem) ||
        (mem.cpu_gpu_memory_size && !cpu_gpu_mem) || !g_frame_buffer_size)
        return false;
    mem.cpu_memory = cpu_mem;
    mem.gpu_memory = gpu_mem;
    mem.cpu_gpu_memory = cpu_gpu_mem;
    mem.cpu_memory_size = cpu_size;
    mem.gpu_memory_size = gpu_size;
    mem.cpu_gpu_memory_size = cpu_gpu_size;
    mem.max_frame_buffer_size = g_frame_buffer_size;
    rc = p_create(&cfg, &mem, &g_decoder);
    LOG("video: sceVideodec2CreateDecoder -> 0x%08x", (unsigned)rc);
    if (rc != 0) {
        g_decoder = nullptr;
        return false;
    }
    g_dec_w = width;
    g_dec_h = height;
    return true;
}

static void request_idr_locked()
{
    g_wait_idr = true;
    g_want_idr = true;
}

static void decode_one(Slot &s, bool more_pending, const uint8_t *config, size_t config_len)
{
    // IDR frames get the SPS/PPS put back in front of them (the streamer strips them).
    const uint8_t *au = s.mem + CONFIG_MAX;
    size_t au_len = s.len;
    if (s.idr && config_len) {
        au -= config_len;
        memcpy((void *)au, config, config_len);
        au_len += config_len;
    }
    Vdec2InputData in{};
    in.this_size = sizeof(in);
    in.au_data = au;
    in.au_size = au_len;
    in.pts = in.dts = s.timestamp_ns;
    in.attached = s.received_us;
    Vdec2FrameBuffer fb{};
    fb.this_size = sizeof(fb);
    fb.frame_buffer = g_frame_buffers[g_fb_cur];
    fb.frame_buffer_size = g_frame_buffer_size;
    Vdec2OutputInfo out{};
    out.this_size = sizeof(out);
    uint64_t t0 = now_us();
    int rc = p_decode(g_decoder, &in, &fb, &out);
    uint64_t t1 = now_us();
    static unsigned logged;
    if (logged < 3 || (rc != 0 && logged < 40)) {
        logged++;
        LOG("video: decode %s %zu bytes -> 0x%08x valid=%u err=%u pics=%u %ux%u pitch %u/%u fmt 0x%x fb=%p %.2f ms",
            s.idr ? "IDR" : "P", au_len, (unsigned)rc, out.is_valid, out.is_error_frame, out.picture_count,
            out.frame_width, out.frame_height, out.frame_pitch, out.frame_pitch_in_bytes, out.frame_format,
            out.frame_buffer, (t1 - t0) / 1000.0);
    }
    if (rc != 0) {
        pthread_mutex_lock(&g_lock);
        g_stats.errors++;
        request_idr_locked();
        pthread_mutex_unlock(&g_lock);
        p_reset(g_decoder);
        return;
    }
    pthread_mutex_lock(&g_lock);
    g_stats.decoded++;
    g_stats.decode_us_avg = (g_stats.decode_us_avg * 15 + (t1 - t0)) / 16;
    pthread_mutex_unlock(&g_lock);
    if (!out.is_valid || more_pending)
        return; // a newer frame is already waiting: only decode this one (references)
    g_fb_cur ^= 1;

    uint32_t fw = out.frame_width, fh = out.frame_height;
    uint32_t pitch = out.frame_pitch_in_bytes ? out.frame_pitch_in_bytes : out.frame_pitch;
    uint32_t eye_w = g_view_w, eye_h = g_view_h;
    if (eye_w * 2 > fw)
        eye_w = fw / 2;
    if (eye_h > fh)
        eye_h = fh;
    if (!alloc_eye_buffers(eye_w, eye_h))
        return;
    int w;
    pthread_mutex_lock(&g_pub_lock);
    for (w = 0; w < EYE_SETS; w++)
        if (w != g_published && w != g_displayed && w != g_prev_displayed)
            break;
    pthread_mutex_unlock(&g_pub_lock);
    const uint8_t *y = (const uint8_t *)out.frame_buffer;
    const uint8_t *uv = y + (size_t)pitch * fh;
    ConvertJob left{y, uv, pitch, 0, eye_w & ~1u, eye_h, g_eye_mem[w][0], g_eye_pitch, g_full_range};
    ConvertJob right = left;
    right.x0 = eye_w;
    right.dst = g_eye_mem[w][1];
    uint64_t t2 = now_us();
    pthread_mutex_lock(&g_job_lock);
    g_job = right;
    unsigned gen = ++g_job_gen;
    pthread_cond_broadcast(&g_job_cond);
    pthread_mutex_unlock(&g_job_lock);
    convert(left);
    pthread_mutex_lock(&g_job_lock);
    while (g_job_done != gen)
        pthread_cond_wait(&g_job_cond, &g_job_lock);
    pthread_mutex_unlock(&g_job_lock);
    uint64_t t3 = now_us();

    pthread_mutex_lock(&g_pub_lock);
    g_published = w;
    g_pub_ts = s.timestamp_ns;
    g_pub_decoded_us = t3;
    g_pub_seq++;
    pthread_mutex_unlock(&g_pub_lock);
    pthread_mutex_lock(&g_lock);
    g_stats.convert_us_avg = (g_stats.convert_us_avg * 15 + (t3 - t2)) / 16;
    pthread_mutex_unlock(&g_lock);
}

static void *decode_thread(void *)
{
    for (;;) {
        pthread_mutex_lock(&g_lock);
        while (g_count == 0)
            pthread_cond_wait(&g_cond, &g_lock);
        Slot s = g_slots[g_head];
        unsigned gen = g_config_gen;
        uint32_t codec = g_codec, vw = g_view_w, vh = g_view_h;
        static uint8_t config[CONFIG_MAX];
        size_t config_len = s.idr ? g_config_len : 0;
        memcpy(config, g_config, config_len);
        pthread_mutex_unlock(&g_lock);

        bool ok = true;
        if (!g_decoder || gen != g_decoder_gen) {
            if (codec != 0) {
                static bool warned;
                if (!warned)
                    LOG("video: codec %u is not supported, set the streamer to H264", codec);
                warned = true;
                ok = false;
            } else {
                ok = create_decoder(vw * 2, vh);
                if (!ok)
                    LOG("video: decoder creation failed");
                g_decoder_gen = gen; // do not retry every frame; a new DecoderConfig retries
            }
        }
        pthread_mutex_lock(&g_lock);
        bool more = g_count > 1;
        pthread_mutex_unlock(&g_lock);
        if (ok && g_decoder)
            decode_one(s, more, config, config_len);
        pthread_mutex_lock(&g_lock);
        // The slot is released only now: its memory was in use until the decode returned.
        if (g_count > 0) {
            g_head = (g_head + 1) % QUEUE_SLOTS;
            g_count--;
        }
        pthread_mutex_unlock(&g_lock);
    }
    return nullptr;
}

// ---------------------------------------------------------------------------------------
// Public API

bool video_init(int module)
{
    struct {
        const char *name;
        void **fn;
    } syms[] = {
        {"sceVideodec2QueryComputeMemoryInfo", (void **)&p_query_compute},
        {"sceVideodec2AllocateComputeQueue", (void **)&p_alloc_compute},
        {"sceVideodec2QueryDecoderMemoryInfo", (void **)&p_query_decoder},
        {"sceVideodec2CreateDecoder", (void **)&p_create},
        {"sceVideodec2DeleteDecoder", (void **)&p_delete},
        {"sceVideodec2Decode", (void **)&p_decode},
        {"sceVideodec2Reset", (void **)&p_reset},
    };
    for (auto &s : syms)
        if (module < 0 || sceKernelDlsym(module, s.name, s.fn) != 0 || !*s.fn) {
            LOG("video: symbol %s not found", s.name);
            return false;
        }
    uint8_t *mem = (uint8_t *)alloc_direct(SLOT_SIZE * QUEUE_SLOTS, 0x10000, MEM_ONION, "frame queue");
    if (!mem)
        return false;
    for (int i = 0; i < QUEUE_SLOTS; i++)
        g_slots[i].mem = mem + SLOT_SIZE * i;
    pthread_t t;
    pthread_create(&t, nullptr, decode_thread, nullptr);
    pthread_create(&t, nullptr, convert_thread, nullptr);
    LOG("video: ready");
    return true;
}

void video_set_stream(uint32_t view_width, uint32_t view_height, bool full_range)
{
    pthread_mutex_lock(&g_lock);
    g_view_w = view_width ? view_width : 960;
    g_view_h = view_height ? view_height : 1056;
    g_full_range = full_range;
    pthread_mutex_unlock(&g_lock);
}

void video_set_decoder_config(uint32_t codec, const uint8_t *config, size_t len)
{
    pthread_mutex_lock(&g_lock);
    bool changed = codec != g_codec || len != g_config_len || memcmp(config, g_config, len) != 0;
    if (len > CONFIG_MAX) {
        LOG("video: decoder config too large (%zu bytes)", len);
        len = 0;
    }
    if (changed) {
        g_codec = codec;
        memcpy(g_config, config, len);
        g_config_len = len;
        g_config_gen++;
        request_idr_locked();
    }
    pthread_mutex_unlock(&g_lock);
    if (changed)
        LOG("video: decoder config codec %u, %zu bytes", codec, len);
}

void video_push_frame(uint64_t timestamp_ns, bool is_idr, const uint8_t *data, size_t len)
{
    pthread_mutex_lock(&g_lock);
    g_stats.received++;
    if (is_idr)
        g_wait_idr = false;
    if (g_wait_idr || g_config_len == 0 || len > SLOT_SIZE - CONFIG_MAX) {
        g_stats.dropped++;
        pthread_mutex_unlock(&g_lock);
        return;
    }
    if (g_count >= QUEUE_SLOTS - 1) { // decoder too far behind: restart from an IDR
        g_stats.dropped++;
        request_idr_locked();
        pthread_mutex_unlock(&g_lock);
        return;
    }
    Slot &s = g_slots[(g_head + g_count) % QUEUE_SLOTS];
    memcpy(s.mem + CONFIG_MAX, data, len);
    s.len = len;
    s.timestamp_ns = timestamp_ns;
    s.received_us = now_us();
    s.idr = is_idr;
    g_count++;
    pthread_cond_signal(&g_cond);
    pthread_mutex_unlock(&g_lock);
}

void video_packet_loss()
{
    pthread_mutex_lock(&g_lock);
    request_idr_locked();
    pthread_mutex_unlock(&g_lock);
}

bool video_want_idr()
{
    pthread_mutex_lock(&g_lock);
    uint64_t t = now_us();
    // The streamer sends DecoderConfig only in reply to RequestIdr, so the first request
    // goes out before any configuration exists. A new need is sent at most every 100 ms
    // (the streamer's minimum IDR interval); while frames are still dropped waiting for the
    // configuration or the IDR, ask again every 500 ms (lost request).
    uint64_t since = t - g_last_idr_request_us;
    bool want = (g_want_idr && since >= 100000) || ((g_wait_idr || !g_config_len) && since >= 500000);
    if (want) {
        g_want_idr = false;
        g_last_idr_request_us = t;
    }
    pthread_mutex_unlock(&g_lock);
    return want;
}

void video_reset()
{
    // Queued frames are left to the decode thread (it owns the head slot); without a
    // configuration they are dropped at the next IDR wait anyway.
    pthread_mutex_lock(&g_lock);
    g_wait_idr = true;
    g_want_idr = false;
    g_config_len = 0;
    g_codec = 0xffffffff;
    pthread_mutex_unlock(&g_lock);
    pthread_mutex_lock(&g_pub_lock);
    g_published = -1;
    pthread_mutex_unlock(&g_pub_lock);
}

bool video_latest(VideoFrame *out)
{
    pthread_mutex_lock(&g_pub_lock);
    if (g_published >= 0 && g_published != g_displayed) {
        g_prev_displayed = g_displayed;
        g_displayed = g_published;
    }
    bool ok = g_displayed >= 0 && g_published >= 0;
    if (ok) {
        out->eye[0] = &g_eye_tex[g_displayed][0];
        out->eye[1] = &g_eye_tex[g_displayed][1];
        out->timestamp_ns = g_pub_ts;
        out->decoded_us = g_pub_decoded_us;
        out->seq = g_pub_seq;
    }
    pthread_mutex_unlock(&g_pub_lock);
    return ok;
}

void video_get_stats(VideoStats *out)
{
    pthread_mutex_lock(&g_lock);
    *out = g_stats;
    pthread_mutex_unlock(&g_lock);
}
