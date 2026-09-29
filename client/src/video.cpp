#include "video.h"

#include <emmintrin.h>
#include <math.h>
#include <pthread.h>
#include <string.h>
#include <sys/resource.h>

#include <orbis/libkernel.h>

#include "foveation.h"
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
static const int MAX_QUEUED = 4;
static const size_t CONFIG_MAX = 1024;       // SPS/PPS, prepended to IDR frames in place
static const size_t SLOT_SIZE = 3 << 20;     // largest access unit accepted (+ CONFIG_MAX)
static const int EYE_SETS = 4;               // published, displayed, previously displayed, writing
// Decoder pipeline depth: at 1 every Decode call waits for its own picture (13-18 ms at
// 1920x1056, too close to the 16.7 ms frame time: frames queued up and added ~100 ms).
// At 2 the library decodes one access unit while returning the previous picture.
static const uint32_t DECODE_DEPTH = 2;

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
static uint32_t g_view_w = 960, g_view_h = 1056;   // one eye as displayed
static uint32_t g_frame_w = 1920, g_frame_h = 1056; // decoded frame (both eyes)
static bool g_full_range = true;
static bool g_ffe;                                  // foveated encoding: frame squeezed
static FoveationAxis g_ffe_x, g_ffe_y;
static unsigned g_stream_gen;                       // bumped by every video_set_stream
static VideoStats g_stats;

// Decoder (decode thread only).
static void *g_compute_queue;
static void *g_decoder;
static unsigned g_decoder_gen;
static uint32_t g_dec_w, g_dec_h;
static const int FRAME_BUFFERS = 3; // being decoded into, waiting for conversion, being converted
static void *g_frame_buffers[FRAME_BUFFERS];
static uint64_t g_frame_buffer_size;

// Decoded picture handed from the decode thread to the conversion thread.
struct Decoded {
    int fb;
    uint32_t width, height, pitch;
    uint64_t timestamp_ns;
};
static pthread_mutex_t g_dec_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_dec_cond = PTHREAD_COND_INITIALIZER;
static Decoded g_pending;
static bool g_has_pending;
static int g_converting_fb = -1;

// Eye buffers.
static uint32_t *g_eye_mem[EYE_SETS][2];
static GnmTexture g_eye_tex[EYE_SETS][2];
static uint32_t g_eye_w, g_eye_h, g_eye_pitch;
static pthread_mutex_t g_pub_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_published = -1, g_displayed = -1, g_prev_displayed = -1;
static uint64_t g_pub_ts, g_pub_decoded_us;
static unsigned g_pub_seq;

static uint64_t now_us() { return sceKernelGetProcessTime(); }

// CPU time of the calling thread (0 if unavailable): tells whether Decode waits for the
// hardware or parses the bitstream on this thread.
static uint64_t thread_cpu_us()
{
    struct rusage ru;
    if (getrusage(1 /* RUSAGE_THREAD */, &ru) != 0)
        return 0;
    return (uint64_t)(ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) * 1000000 + ru.ru_utime.tv_usec + ru.ru_stime.tv_usec;
}

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

// Eye buffers are allocated once for the largest stream (160% of the panel) and only
// re-described when a new session uses another size.
static const uint32_t EYE_MAX_W = 1536, EYE_MAX_H = 1728;

struct ConvertJob {
    const uint8_t *y, *uv;
    uint32_t pitch, x0, w, h;
    uint32_t *dst;
    uint32_t dst_pitch;
    bool full_range;
};

static inline uint32_t clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : (uint32_t)v; }

static void convert_scalar(const ConvertJob &j);

// SSE2, 16-bit fixed point with 6 fractional bits.
// Products take the high half of (v << 8) * (c * 16384), which is v * c * 64.
// Coefficients of 2.0 and more do not fit in int16: they are halved and the product doubled.
struct Coeffs {
    __m128i c128, cyo, cys, crv, cgu, cgv, cbu, round, alpha;
};

static void make_coeffs(Coeffs &k, bool full_range)
{
    int16_t ys, yo, rv, gu, gv, bu_half;
    if (full_range) {
        ys = 16384; yo = 0; rv = 25802; gu = 3069; gv = 7669; bu_half = 15201;       // 1.0, 1.5748, 0.1873, 0.4681, 1.8556/2
    } else {
        ys = 19071; yo = 16; rv = 29377; gu = 3490; gv = 8733; bu_half = 17302;      // 1.164, 1.793, 0.213, 0.533, 2.112/2
    }
    k.c128 = _mm_set1_epi16(128);
    k.cyo = _mm_set1_epi16(yo);
    k.cys = _mm_set1_epi16(ys);
    k.crv = _mm_set1_epi16(rv);
    k.cgu = _mm_set1_epi16(gu);
    k.cgv = _mm_set1_epi16(gv);
    k.cbu = _mm_set1_epi16(bu_half);
    k.round = _mm_set1_epi16(32);
    k.alpha = _mm_set1_epi8((char)0x80); // same alpha as the lobby
}

// 8 pixels per iteration, of ROWS luma rows sharing one chroma row. w is a multiple of 8.
template <int ROWS>
static inline void convert_span(const Coeffs &k, const uint8_t *y0, const uint8_t *y1, const uint8_t *uvp,
                                uint32_t *d0, uint32_t *d1, uint32_t w)
{
    const __m128i zero = _mm_setzero_si128();
    for (uint32_t x = 0; x < w; x += 8) {
        // Chroma of 8 pixels: 4 (U, V) pairs, each shared by 2 pixels.
        __m128i uv = _mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i *)(uvp + x)), zero);
        uv = _mm_sub_epi16(uv, k.c128);                                // U0 V0 U1 V1 ...
        __m128i u = _mm_srai_epi32(_mm_slli_epi32(uv, 16), 16);         // 4 x i32
        __m128i v = _mm_srai_epi32(uv, 16);
        u = _mm_packs_epi32(u, u);
        v = _mm_packs_epi32(v, v);
        u = _mm_slli_epi16(_mm_unpacklo_epi16(u, u), 8);               // U0 U0 U1 U1 .. << 8
        v = _mm_slli_epi16(_mm_unpacklo_epi16(v, v), 8);
        __m128i cr = _mm_mulhi_epi16(v, k.crv);
        __m128i cg = _mm_add_epi16(_mm_mulhi_epi16(u, k.cgu), _mm_mulhi_epi16(v, k.cgv));
        __m128i cb = _mm_slli_epi16(_mm_mulhi_epi16(u, k.cbu), 1);
        for (int r = 0; r < ROWS; r++) {
            const uint8_t *yp = r ? y1 : y0;
            // Luma: (Y - offset) << 8 needs 16 unsigned bits, hence the unsigned multiply
            // (Y below the limited-range black level saturates to 0).
            __m128i yv = _mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i *)(yp + x)), zero);
            yv = _mm_mulhi_epu16(_mm_slli_epi16(_mm_subs_epu16(yv, k.cyo), 8), k.cys);
            yv = _mm_add_epi16(yv, k.round);
            __m128i R = _mm_srai_epi16(_mm_adds_epi16(yv, cr), 6);
            __m128i G = _mm_srai_epi16(_mm_subs_epi16(yv, cg), 6);
            __m128i B = _mm_srai_epi16(_mm_adds_epi16(yv, cb), 6);
            __m128i b8 = _mm_packus_epi16(B, B), g8 = _mm_packus_epi16(G, G), r8 = _mm_packus_epi16(R, R);
            __m128i bg = _mm_unpacklo_epi8(b8, g8), ra = _mm_unpacklo_epi8(r8, k.alpha);
            uint32_t *d = r ? d1 : d0;
            _mm_storeu_si128((__m128i *)(d + x), _mm_unpacklo_epi16(bg, ra));
            _mm_storeu_si128((__m128i *)(d + x + 4), _mm_unpackhi_epi16(bg, ra));
        }
    }
}

static void convert(const ConvertJob &j)
{
    if (j.w % 8 || j.x0 % 2) {
        convert_scalar(j);
        return;
    }
    Coeffs k;
    make_coeffs(k, j.full_range);
    for (uint32_t row = 0; row < j.h; row += 2) {
        const uint8_t *y0 = j.y + (size_t)row * j.pitch + j.x0;
        const uint8_t *uvp = j.uv + (size_t)(row / 2) * j.pitch + j.x0;
        uint32_t *d0 = j.dst + (size_t)row * j.dst_pitch;
        if (row + 1 < j.h)
            convert_span<2>(k, y0, y0 + j.pitch, uvp, d0, d0 + j.dst_pitch, j.w);
        else
            convert_span<1>(k, y0, y0, uvp, d0, d0, j.w);
    }
}

static void convert_scalar(const ConvertJob &j)
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

// ---------------------------------------------------------------------------------------
// Foveated encoding: expansion of the squeezed eyes (see foveation.h)

// Source of every output column (per eye: the right eye is mirrored) and row: the left or
// upper source pixel and a 6-bit weight of the next one. Columns where the source is a
// straight 1:1 copy (the center) form runs copied with memcpy.
struct ColumnRun {
    uint32_t x0, x1;
    bool copy;
};
static const int MAX_RUNS = 32;
struct FfeLut {
    uint32_t src_w, src_h; // one compressed eye
    int32_t col[2][EYE_MAX_W];
    __m128i col_weights[2][EYE_MAX_W]; // (64 - w) x4, w x4
    ColumnRun runs[2][MAX_RUNS];
    int nruns[2];
    int32_t row[EYE_MAX_H];
    uint8_t row_weight[EYE_MAX_H];
};
static FfeLut g_lut;

static void source_position(double s, uint32_t n, int32_t *index, int *weight)
{
    int i = (int)floor(s);
    int w = (int)lround((s - i) * 64.0);
    if (w == 64) {
        i++;
        w = 0;
    }
    if (i < 0) {
        i = 0;
        w = 0;
    }
    if (i >= (int)n - 1) { // last pixel, read as the right one of the last pair
        i = (int)n - 2;
        w = 64;
    }
    *index = i;
    *weight = w;
}

static void build_ffe_lut(uint32_t w, uint32_t h, const FoveationAxis &ax, const FoveationAxis &ay)
{
    FfeLut &l = g_lut;
    l.src_w = ax.compressed;
    l.src_h = ay.compressed;
    static uint8_t weight[EYE_MAX_W];
    uint32_t copied = 0;
    for (int e = 0; e < 2; e++) {
        for (uint32_t x = 0; x < w; x++) {
            double u = (x + 0.5) / w;
            if (e)
                u = 1.0 - u;
            double v = foveation_map(&ax, u);
            if (e)
                v = 1.0 - v;
            int32_t i0;
            int wt;
            source_position(v * l.src_w - 0.5, l.src_w, &i0, &wt);
            l.col[e][x] = i0;
            weight[x] = (uint8_t)wt;
            short a = (short)(64 - wt), b = (short)wt;
            l.col_weights[e][x] = _mm_setr_epi16(a, a, a, a, b, b, b, b);
        }
        // Copy runs: weight 0 and consecutive sources, at least 16 columns long.
        int n = 0;
        uint32_t x = 0, interp_from = 0;
        while (x < w) {
            uint32_t end = x;
            while (end < w && weight[end] == 0 && (end == x || l.col[e][end] == l.col[e][end - 1] + 1))
                end++;
            if (end - x >= 16 && n < MAX_RUNS - 2) {
                if (interp_from < x)
                    l.runs[e][n++] = ColumnRun{interp_from, x, false};
                l.runs[e][n++] = ColumnRun{x, end, true};
                if (e == 0)
                    copied += end - x;
                interp_from = end;
            }
            x = end > x ? end : x + 1;
        }
        if (interp_from < w)
            l.runs[e][n++] = ColumnRun{interp_from, w, false};
        l.nruns[e] = n;
    }
    uint32_t direct = 0;
    for (uint32_t y = 0; y < h; y++) {
        int32_t i0;
        int wt;
        source_position(foveation_map(&ay, (y + 0.5) / h) * l.src_h - 0.5, l.src_h, &i0, &wt);
        if (wt == 64) { // exactly the next row
            i0++;
            wt = 0;
        }
        l.row[y] = i0;
        l.row_weight[y] = (uint8_t)wt;
        direct += wt == 0;
    }
    LOG("video: foveation %ux%u per eye from %ux%u: %u columns and %u rows copied 1:1, %d column runs",
        w, h, l.src_w, l.src_h, copied, direct, l.nruns[0]);
}

struct FfeJob {
    const uint8_t *y, *uv; // decoded frame planes
    uint32_t pitch;
    uint32_t src_x0; // first column of this eye in the decoded frame
    int eye;
    uint32_t row_begin, row_end;
    uint32_t *dst;
    uint32_t dst_pitch;
    bool full_range;
};

// Each eye is converted in three bands: by the conversion thread and five helpers (4 jobs
// took ~5 ms per frame with foveated encoding; the PS4 has cores to spare while streaming).
static const int CONVERT_JOBS = 6;
static const int EYE_BANDS = CONVERT_JOBS / 2;
// Conversion is on the latency path: above the default priority (700; 256 is the highest,
// used by the decoder) so audio, network or tracker work does not preempt a band.
static const int CONVERT_PRIORITY = 320;

static void raise_priority(const char *who)
{
    int rc = scePthreadSetprio(scePthreadSelf(), CONVERT_PRIORITY);
    static bool logged;
    if (!logged || rc != 0)
        LOG("video: %s priority %d -> 0x%08x", who, CONVERT_PRIORITY, (unsigned)rc);
    logged = true;
}
static uint32_t g_row_buf[CONVERT_JOBS][3][EYE_MAX_W + 32] __attribute__((aligned(64)));

static void blend_rows(const uint32_t *a, const uint32_t *b, int w, uint32_t *out, uint32_t n)
{
    const __m128i zero = _mm_setzero_si128(), round = _mm_set1_epi16(32);
    const __m128i wa = _mm_set1_epi16((short)(64 - w)), wb = _mm_set1_epi16((short)w);
    for (uint32_t i = 0; i < n; i += 4) {
        __m128i pa = _mm_loadu_si128((const __m128i *)(a + i)), pb = _mm_loadu_si128((const __m128i *)(b + i));
        __m128i lo = _mm_add_epi16(_mm_mullo_epi16(_mm_unpacklo_epi8(pa, zero), wa),
                                   _mm_mullo_epi16(_mm_unpacklo_epi8(pb, zero), wb));
        __m128i hi = _mm_add_epi16(_mm_mullo_epi16(_mm_unpackhi_epi8(pa, zero), wa),
                                   _mm_mullo_epi16(_mm_unpackhi_epi8(pb, zero), wb));
        lo = _mm_srli_epi16(_mm_add_epi16(lo, round), 6);
        hi = _mm_srli_epi16(_mm_add_epi16(hi, round), 6);
        _mm_storeu_si128((__m128i *)(out + i), _mm_packus_epi16(lo, hi));
    }
}

static void expand_row(const uint32_t *src, uint32_t *dst, int eye)
{
    const FfeLut &l = g_lut;
    const __m128i zero = _mm_setzero_si128(), round = _mm_set1_epi16(32);
    for (int r = 0; r < l.nruns[eye]; r++) {
        const ColumnRun &run = l.runs[eye][r];
        if (run.copy) {
            memcpy(dst + run.x0, src + l.col[eye][run.x0], (run.x1 - run.x0) * 4);
            continue;
        }
        const int32_t *col = l.col[eye];
        const __m128i *wv = l.col_weights[eye];
        for (uint32_t x = run.x0; x < run.x1; x++) {
            // Two neighbouring source pixels, weighted, then the halves summed.
            __m128i p = _mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i *)(src + col[x])), zero);
            p = _mm_mullo_epi16(p, wv[x]);
            p = _mm_add_epi16(_mm_add_epi16(p, _mm_srli_si128(p, 8)), round);
            dst[x] = (uint32_t)_mm_cvtsi128_si32(_mm_packus_epi16(_mm_srli_epi16(p, 6), zero));
        }
    }
}

// Rows of the compressed eye are converted to BGRA once each (two are cached), blended
// vertically when needed, then expanded horizontally into the eye buffer.
static void ffe_band(const FfeJob &j, int slot)
{
    const FfeLut &l = g_lut;
    Coeffs k;
    make_coeffs(k, j.full_range);
    uint32_t *cache[2] = {g_row_buf[slot][0], g_row_buf[slot][1]};
    int cached[2] = {-1, -1};
    uint32_t *blend = g_row_buf[slot][2];
    auto row = [&](int r) -> const uint32_t * {
        if (cached[0] == r)
            return cache[0];
        if (cached[1] == r)
            return cache[1];
        int s = cached[0] < cached[1] ? 0 : 1; // rows only move down: drop the upper one
        const uint8_t *yr = j.y + (size_t)r * j.pitch + j.src_x0;
        const uint8_t *uvr = j.uv + (size_t)(r / 2) * j.pitch + j.src_x0;
        convert_span<1>(k, yr, yr, uvr, cache[s], cache[s], l.src_w);
        cached[s] = r;
        return cache[s];
    };
    for (uint32_t y = j.row_begin; y < j.row_end; y++) {
        int r = l.row[y], w = l.row_weight[y];
        const uint32_t *src = row(r);
        if (w) {
            const uint32_t *next = row(r + 1);
            blend_rows(src, next, w, blend, l.src_w);
            src = blend;
        }
        expand_row(src, j.dst + (size_t)y * j.dst_pitch, j.eye);
    }
}

// Conversion jobs of one frame: job 0 runs on the conversion thread, the others on helpers.
struct Job {
    bool ffe;
    ConvertJob conv;
    FfeJob fov;
};
static pthread_mutex_t g_job_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_job_cond = PTHREAD_COND_INITIALIZER;
static Job g_jobs[CONVERT_JOBS];
static unsigned g_job_gen, g_jobs_done;

static void run_job(int i)
{
    if (g_jobs[i].ffe)
        ffe_band(g_jobs[i].fov, i);
    else
        convert(g_jobs[i].conv);
}

static void *convert_thread(void *arg)
{
    int index = (int)(intptr_t)arg;
    raise_priority("conversion helper");
    unsigned seen = 0;
    for (;;) {
        pthread_mutex_lock(&g_job_lock);
        while (g_job_gen == seen)
            pthread_cond_wait(&g_job_cond, &g_job_lock);
        seen = g_job_gen;
        pthread_mutex_unlock(&g_job_lock);
        run_job(index);
        pthread_mutex_lock(&g_job_lock);
        g_jobs_done++;
        pthread_cond_broadcast(&g_job_cond);
        pthread_mutex_unlock(&g_job_lock);
    }
    return nullptr;
}

static bool alloc_eye_buffers(uint32_t w, uint32_t h)
{
    if (g_eye_mem[0][0] && w == g_eye_w && h == g_eye_h)
        return true;
    if (w > EYE_MAX_W || h > EYE_MAX_H) {
        LOG("video: eye size %ux%u above the %ux%u maximum", w, h, EYE_MAX_W, EYE_MAX_H);
        return false;
    }
    const size_t each = ((size_t)EYE_MAX_W * EYE_MAX_H * 4 + 0xffff) & ~(size_t)0xffff;
    if (!g_eye_mem[0][0]) {
        uint8_t *mem = (uint8_t *)alloc_direct(each * EYE_SETS * 2, 0x10000, MEM_ONION, "eye buffers");
        if (!mem)
            return false;
        for (int s = 0; s < EYE_SETS; s++)
            for (int e = 0; e < 2; e++)
                g_eye_mem[s][e] = (uint32_t *)(mem + each * (s * 2 + e));
    }
    uint32_t pitch = (w + 63) & ~63u;
    for (int s = 0; s < EYE_SETS; s++)
        for (int e = 0; e < 2; e++) {
            memset(g_eye_mem[s][e], 0, each);
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

// Timestamps of the access units inside the decoder, oldest first: with a pipeline depth
// of N, a picture comes out N-1 Decode calls after its access unit went in.
static uint64_t g_ts_fifo[16];
static int g_ts_head, g_ts_count;

static void ts_fifo_clear() { g_ts_head = g_ts_count = 0; }

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
    cfg.decode_pipeline_depth = DECODE_DEPTH;
    cfg.compute_queue = g_compute_queue;
    cfg.cpu_affinity_mask = 0x3f;
    cfg.cpu_thread_priority = 256; // highest allowed: decoding is on the latency path
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
        bool all = true;
        for (int i = 0; i < FRAME_BUFFERS; i++)
            all = (g_frame_buffers[i] = alloc_direct(mem.max_frame_buffer_size, 0x10000, MEM_ONION, "frame buffer")) && all;
        g_frame_buffer_size = all ? mem.max_frame_buffer_size : 0;
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
    ts_fifo_clear();
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

static void decode_one(Slot &s, const uint8_t *config, size_t config_len)
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
    // A frame buffer that is neither waiting for conversion nor being converted.
    int fb_index = 0;
    pthread_mutex_lock(&g_dec_lock);
    while (fb_index < FRAME_BUFFERS - 1 &&
           ((g_has_pending && fb_index == g_pending.fb) || fb_index == g_converting_fb))
        fb_index++;
    pthread_mutex_unlock(&g_dec_lock);
    Vdec2FrameBuffer fb{};
    fb.this_size = sizeof(fb);
    fb.frame_buffer = g_frame_buffers[fb_index];
    fb.frame_buffer_size = g_frame_buffer_size;
    Vdec2OutputInfo out{};
    out.this_size = sizeof(out);
    uint64_t t0 = now_us(), c0 = thread_cpu_us();
    int rc = p_decode(g_decoder, &in, &fb, &out);
    uint64_t t1 = now_us(), c1 = thread_cpu_us();
    if (rc == 0 && g_ts_count < 16)
        g_ts_fifo[(g_ts_head + g_ts_count++) % 16] = s.timestamp_ns;
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
        ts_fifo_clear();
        return;
    }
    pthread_mutex_lock(&g_lock);
    g_stats.decoded++;
    g_stats.decode_us_avg = (g_stats.decode_us_avg * 15 + (t1 - t0)) / 16;
    if (c0 && c1 >= c0)
        g_stats.decode_cpu_us_avg = (g_stats.decode_cpu_us_avg * 15 + (c1 - c0)) / 16;
    pthread_mutex_unlock(&g_lock);
    if (!out.is_valid)
        return;
    uint64_t ts = s.timestamp_ns;
    if (g_ts_count > 0) {
        ts = g_ts_fifo[g_ts_head];
        g_ts_head = (g_ts_head + 1) % 16;
        g_ts_count--;
    }
    // Hand the picture to the conversion thread; an older one it has not started is
    // replaced (its frame buffer becomes free again). Decoding and conversion overlap.
    pthread_mutex_lock(&g_dec_lock);
    g_pending = Decoded{fb_index, out.frame_width, out.frame_height,
                        out.frame_pitch_in_bytes ? out.frame_pitch_in_bytes : out.frame_pitch, ts};
    g_has_pending = true;
    pthread_cond_signal(&g_dec_cond);
    pthread_mutex_unlock(&g_dec_lock);
}

// Conversion thread: NV12 of the newest decoded picture -> BGRA eye buffers (left eye
// here, right eye on the helper thread), then publication to the render loop.
static void *convert_main_thread(void *)
{
    raise_priority("conversion");
    for (;;) {
        pthread_mutex_lock(&g_dec_lock);
        while (!g_has_pending)
            pthread_cond_wait(&g_dec_cond, &g_dec_lock);
        Decoded d = g_pending;
        g_has_pending = false;
        g_converting_fb = d.fb;
        pthread_mutex_unlock(&g_dec_lock);

        pthread_mutex_lock(&g_lock);
        uint32_t eye_w = g_view_w, eye_h = g_view_h;
        bool full_range = g_full_range, ffe = g_ffe;
        FoveationAxis ax = g_ffe_x, ay = g_ffe_y;
        unsigned stream_gen = g_stream_gen;
        pthread_mutex_unlock(&g_lock);
        if (ffe && (d.width < ax.compressed * 2 || d.height < ay.compressed)) {
            static bool warned;
            if (!warned)
                LOG("video: decoded %ux%u is smaller than the foveated %ux%u frame, shown without expansion",
                    d.width, d.height, ax.compressed * 2, ay.compressed);
            warned = true;
            ffe = false;
        }
        if (ffe) {
            static unsigned lut_for; // stream generation + 1 the LUT was built for
            if (lut_for != stream_gen + 1) {
                build_ffe_lut(eye_w, eye_h, ax, ay);
                lut_for = stream_gen + 1;
            }
        } else {
            if (eye_w * 2 > d.width)
                eye_w = d.width / 2;
            if (eye_h > d.height)
                eye_h = d.height;
        }
        if (alloc_eye_buffers(eye_w, eye_h)) {
            int w;
            pthread_mutex_lock(&g_pub_lock);
            for (w = 0; w < EYE_SETS; w++)
                if (w != g_published && w != g_displayed && w != g_prev_displayed)
                    break;
            pthread_mutex_unlock(&g_pub_lock);
            const uint8_t *y = (const uint8_t *)g_frame_buffers[d.fb];
            const uint8_t *uv = y + (size_t)d.pitch * d.height;
            // Each eye in EYE_BANDS horizontal bands (even split rows keep chroma pairs).
            for (int e = 0; e < 2; e++)
                for (int band = 0; band < EYE_BANDS; band++) {
                    Job &job = g_jobs[e * EYE_BANDS + band];
                    auto split = [&](int b) -> uint32_t {
                        uint32_t r = eye_h * (uint32_t)b / EYE_BANDS;
                        return b == EYE_BANDS ? eye_h : ffe ? r : r & ~1u;
                    };
                    uint32_t r0 = split(band), r1 = split(band + 1);
                    job.ffe = ffe;
                    if (ffe)
                        job.fov = FfeJob{y, uv, d.pitch, e ? ax.compressed : 0, e, r0, r1,
                                         g_eye_mem[w][e], g_eye_pitch, full_range};
                    else
                        job.conv = ConvertJob{y + (size_t)r0 * d.pitch, uv + (size_t)(r0 / 2) * d.pitch, d.pitch,
                                              e ? eye_w : 0, eye_w & ~1u, r1 - r0,
                                              g_eye_mem[w][e] + (size_t)r0 * g_eye_pitch, g_eye_pitch, full_range};
                }
            uint64_t t0 = now_us();
            pthread_mutex_lock(&g_job_lock);
            g_jobs_done = 0;
            g_job_gen++;
            pthread_cond_broadcast(&g_job_cond);
            pthread_mutex_unlock(&g_job_lock);
            run_job(0);
            pthread_mutex_lock(&g_job_lock);
            while (g_jobs_done < CONVERT_JOBS - 1)
                pthread_cond_wait(&g_job_cond, &g_job_lock);
            pthread_mutex_unlock(&g_job_lock);
            uint64_t t1 = now_us();

            pthread_mutex_lock(&g_pub_lock);
            g_published = w;
            g_pub_ts = d.timestamp_ns;
            g_pub_decoded_us = t1;
            g_pub_seq++;
            pthread_mutex_unlock(&g_pub_lock);
            pthread_mutex_lock(&g_lock);
            g_stats.convert_us_avg = (g_stats.convert_us_avg * 15 + (t1 - t0)) / 16;
            pthread_mutex_unlock(&g_lock);
        }
        pthread_mutex_lock(&g_dec_lock);
        g_converting_fb = -1;
        pthread_mutex_unlock(&g_dec_lock);
    }
    return nullptr;
}

static void *decode_thread(void *)
{
    for (;;) {
        pthread_mutex_lock(&g_lock);
        while (g_count == 0)
            pthread_cond_wait(&g_cond, &g_lock);
        Slot s = g_slots[g_head];
        unsigned gen = g_config_gen;
        uint32_t codec = g_codec, fw = g_frame_w, fh = g_frame_h;
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
                ok = create_decoder(fw, fh);
                if (!ok)
                    LOG("video: decoder creation failed");
                g_decoder_gen = gen; // do not retry every frame; a new DecoderConfig retries
            }
        }
        if (ok && g_decoder)
            decode_one(s, config, config_len);
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
    for (int i = 1; i < CONVERT_JOBS; i++)
        pthread_create(&t, nullptr, convert_thread, (void *)(intptr_t)i);
    pthread_create(&t, nullptr, convert_main_thread, nullptr);
    LOG("video: ready");
    return true;
}

void video_set_stream(uint32_t view_width, uint32_t view_height, bool full_range, const FoveationSettings *ffe)
{
    pthread_mutex_lock(&g_lock);
    g_view_w = view_width ? view_width : 960;
    g_view_h = view_height ? view_height : 1056;
    g_full_range = full_range;
    g_ffe = ffe != nullptr;
    if (ffe) {
        foveation_axis(&g_ffe_x, g_view_w, ffe->center_size_x, ffe->center_shift_x, ffe->edge_ratio_x);
        foveation_axis(&g_ffe_y, g_view_h, ffe->center_size_y, ffe->center_shift_y, ffe->edge_ratio_y);
        g_frame_w = g_ffe_x.compressed * 2;
        g_frame_h = g_ffe_y.compressed;
    } else {
        g_frame_w = g_view_w * 2;
        g_frame_h = g_view_h;
    }
    g_stream_gen++;
    uint32_t fw = g_frame_w, fh = g_frame_h;
    pthread_mutex_unlock(&g_lock);
    LOG("video: %ux%u per eye, decoded frame %ux%u%s", view_width, view_height, fw, fh,
        ffe ? " (foveated encoding)" : "");
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
    // More than MAX_QUEUED frames waiting (~67 ms at 60 Hz) would only add latency (up to
    // 190 ms was seen in busy Beat Saber scenes): drop them and restart from an IDR.
    if (g_count >= MAX_QUEUED) {
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
    g_stats.bytes_avg = (g_stats.bytes_avg * 15 + len) / 16;
    if ((unsigned)g_count > g_stats.queue_max)
        g_stats.queue_max = g_count;
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
    g_stats.queue_max = 0;
    pthread_mutex_unlock(&g_lock);
}

bool video_wait_new(unsigned after_seq, uint32_t timeout_us)
{
    uint64_t end = now_us() + timeout_us;
    for (;;) {
        pthread_mutex_lock(&g_pub_lock);
        bool fresh = g_published >= 0 && g_pub_seq != after_seq;
        pthread_mutex_unlock(&g_pub_lock);
        if (fresh)
            return true;
        if (now_us() >= end)
            return false;
        sceKernelUsleep(500);
    }
}
