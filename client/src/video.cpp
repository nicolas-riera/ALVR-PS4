#include "video.h"

#include <tmmintrin.h> // SSSE3 (the Jaguar CPU has it; -march=btver2)
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

// Frames waiting for the decoder. The decoder finishes a frame every ~9 ms at 80 Mbps
// (110 fps), barely above the 90 fps stream, so frames that arrive bunched (Wi-Fi, PC)
// wait a moment. With at most 4 waiting, such bursts dropped a frame, and every drop froze
// the picture until the IDR frame requested came back (up to 32 drops per 5 s in a game,
// 2026-09-29 logs). Now a burst waits (up to MAX_QUEUED); only a backlog that lasts
// (BACKLOG_US with BACKLOG_FRAMES or more waiting: the decoder cannot keep up) or a full
// queue drops frames.
static const int QUEUE_SLOTS = 12;
static const int MAX_QUEUED = 10;
static const int BACKLOG_FRAMES = 4;
static const uint64_t BACKLOG_US = 1000000;
static const size_t CONFIG_MAX = 1024;       // SPS/PPS, prepended to IDR frames in place
static const size_t SLOT_SIZE = 3 << 20;     // largest access unit accepted (+ CONFIG_MAX)
// Converted frames wait in a small FIFO for the display (READY_MAX), then are displayed; the
// previously displayed set may still be read by the compositor pass in progress.
static const int READY_MAX = 5;
static const int EYE_SETS = READY_MAX + 3;   // ready, displayed, previously displayed, writing
// Paced display (video_next). The PC sends one frame per period of its own clock (90.00
// fps), very regularly; the network, decoding and conversion add jitter. Each converted
// frame gets a due time on a schedule that advances exactly one PC frame period per frame:
// it follows (at most +0.1 ms / -0.02 ms per frame) the earliest-arrival line (one point
// per period, following the frames that arrive earliest) plus a margin, the 99th percentile
// of how late frames came behind that line over the last ~11 s (+0.5 ms). (The latest of
// them, first used, sat at a full period in games: every PC or network hiccup counted.) At
// each decision point (just before a compositor pass, see main.cpp) the newest frame that is
// due is shown and older ones are skipped, with a small hysteresis (PACING_HYSTERESIS_US). Frames thus wait the jitter margin plus their phase to the next pass
// (0-11 ms), and the choice follows the smooth schedule, not each frame's jittery arrival:
// no repeat and skip pairs when a frame is ready right around a decision point. The PC's
// clock runs slightly faster than the headset (90.00 against 89.91 Hz): once per ~11 s two
// frames are due at one decision and one is skipped. A frame later than the margin makes a
// repeat (the margin then covers it for the next ~11 s).
// Simulated on the arrival jitter of the hardware logs (tools: scratch simulation): frames
// wait 12-15 ms on average against 19.5-20 ms with a reserve of one whole frame (and ~30 ms
// with the adaptive 1-3 reserve of the first test: ~80 ms from tracking to photon, against
// ~58 ms with 1); 2-3 repeats per minute on a steady network, plus one per lost frame.
static const int JITTER_SAMPLES = 1024; // ~11 s at 90 fps
static const uint64_t MARGIN_EXTRA_US = 500, MARGIN_INITIAL_US = 3000;
static const uint64_t SCHEDULE_UP_US = 100, SCHEDULE_DOWN_US = 20, LINE_CREEP_US = 10; // per frame
// Decision hysteresis. The decision time jitters by a few ms (the loop's lead before the
// pass moves between 1 and 6 ms, plus its wake-up), so frames due right around it came one
// decision late now and then, and the next decision skipped one: a repeat and skip pair
// each time (hardware logs: 20-40 per 5 s at 90 fps). Now the oldest frame waiting is shown
// when due within PACING_HYSTERESIS_US after the decision, and frames are skipped only for
// a newer one due more than that before it. Simulated (scratch decision_sim, the real
// pacing_due, the loop's lead and wake-up jitter): 5.7 pairs per 5 s -> 0 on a steady
// network, 22 -> 15 with frequent 5-40 ms delays, for 0.2-0.6 ms more waiting on average.
static const uint64_t PACING_HYSTERESIS_US = 1500;
// Decoder pipeline depth: a Decode call returns the picture of the access unit given
// depth - 1 calls before, and blocks until it is ready. A picture takes ~20 ms from its
// access unit to being ready, although the decoder finishes one every 7-10 ms: at 1 every
// call waited for its own picture; at 2 each call waited ~11 ms at 90 fps, just over the
// 11.1 ms frame time (queue full, drops, 45 ms added). At 3 the calls do not wait: 90 fps
// without drops up to 130 Mbps, 28 ms from arrival to display (video bench, 2026-09-29);
// 4 only adds a frame of latency.
// Depth 2 on small frames (1536x832: 17 ms instead of 23 ms at depth 3, fine in the bench)
// made the pictures come out irregularly in a game: 40-80 repeated and skipped frames per
// 5 s against 12-35 at depth 3 (2026-09-30), visible judder. Depth 3 stays.
static const uint32_t DECODE_DEPTH = 3;
// At 60 fps depth 2 is enough (each call waits ~3 ms: the 16.7 ms frame time is well above
// what the decoder needs; it was the depth before 90 Hz), and depth 3 would hold every picture
// a whole 16.7 ms frame longer.
static const uint32_t DECODE_DEPTH_60FPS = 2;
static uint32_t g_decode_depth = DECODE_DEPTH; // video bench option
static uint32_t g_use_depth = DECODE_DEPTH;    // depth the next decoder is created with
static uint32_t g_stream_period_us = 11111;    // stream frame period (video_set_frame_period), under g_lock

struct Slot {
    uint8_t *mem; // CONFIG_MAX bytes of headroom, then the frame
    size_t len;
    uint64_t timestamp_ns;
    uint64_t received_us;
    bool idr;
    unsigned epoch; // g_epoch when queued
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
// Bumped by video_reset and by a pause or resume: frames queued, decoded or converted in an
// older epoch are dropped, so a picture from before is never shown again.
static unsigned g_epoch;
static VideoStats g_stats;

// Decoder (decode thread only).
static void *g_compute_queue;
static void *g_decoder;
static unsigned g_decoder_gen;
static uint32_t g_dec_w, g_dec_h, g_dec_depth;
static const int FRAME_BUFFERS = 3; // being decoded into, waiting for conversion, being converted
static void *g_frame_buffers[FRAME_BUFFERS];
static uint64_t g_frame_buffer_size;

// Decoded picture handed from the decode thread to the conversion thread.
struct Decoded {
    int fb;
    uint32_t width, height, pitch;
    uint64_t timestamp_ns;
    uint64_t received_us; // when the access unit was queued (video bench latency, ALVR statistics)
    uint64_t picture_us;  // when the decoder returned the picture
    unsigned epoch;
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
struct Ready {
    int set;
    uint64_t timestamp_ns, decoded_us, received_us, picture_us;
    uint64_t due_us; // may be shown from then (paced display)
};
static Ready g_ready[READY_MAX]; // oldest first
static int g_ready_count;
static bool g_stream_valid;      // a frame was published since the last reset
static unsigned g_pub_epoch;     // g_epoch, as seen by the publication side
static int g_displayed = -1, g_prev_displayed = -1;
static bool g_disp_valid;        // g_displayed was taken since the last reset (else it is only kept from reuse)
static uint64_t g_disp_ts, g_disp_decoded_us, g_disp_received_us, g_disp_picture_us;
static unsigned g_disp_seq;      // increments with every frame taken for display
static unsigned g_pub_seq;
static unsigned g_ready_overflow, g_ready_trimmed; // frames never displayed (FIFO full / trimmed)
static struct {
    uint64_t period_us = 11111; // PC frame period (video_set_frame_period)
    bool line_valid;
    uint64_t line_us;           // earliest-arrival line at the last frame
    uint64_t last_ready_us, last_ts_ns;
    uint32_t lateness[JITTER_SAMPLES]; // behind the line, microseconds
    int lateness_count, lateness_head;
    uint64_t margin_us = MARGIN_INITIAL_US;
    uint64_t schedule_us;       // due time of the last frame
    uint64_t hold_us;           // how long the last frame is held after its conversion (log)
    uint64_t last_decision_us;
    unsigned late;              // frames shown after the decision point they were due at
    unsigned resyncs;           // safety net used (should stay 0)
} g_pacing;

// Due time of a frame converted at `ready` (tracking timestamp ts_ns), under g_pub_lock.
static uint64_t pacing_due(uint64_t ready, uint64_t ts_ns)
{
    auto &pc = g_pacing;
    const uint64_t T = pc.period_us;
    uint64_t n = 0; // PC frame periods since the last frame (0: the schedule starts again)
    if (!pc.line_valid || ready - pc.last_ready_us > 300000) { // first frame, or after a stall
        pc.line_valid = true;
        pc.line_us = ready;
        pc.lateness_count = pc.lateness_head = 0;
        pc.margin_us = MARGIN_INITIAL_US;
    } else {
        // PC frame periods since the last frame, from the time since the line's last point,
        // less half the margin (frames come up to the margin behind the line: a frame 6 ms
        // late was counted for two periods). A frame that seems two or more periods on but
        // whose tracking timestamp moved by less than 1.5 periods is a late frame, not one
        // after a lost frame. (The timestamps follow the PS4's tracking uplink, not the PC's
        // frames, so they only settle that case: counting periods from them pushed the
        // schedule a period ahead at each double uplink gap, until no frame was ever due;
        // hardware test, the video stalled and the lobby came back.)
        const uint64_t offset = pc.margin_us / 2 < T / 2 ? pc.margin_us / 2 : T / 2;
        n = ready > pc.line_us + offset ? (ready - pc.line_us - offset + T / 2) / T : 1;
        if (n < 1)
            n = 1;
        if (n >= 2 && ts_ns > pc.last_ts_ns && pc.last_ts_ns && ts_ns - pc.last_ts_ns < T * 1500)
            n = 1;
        const uint64_t predicted = pc.line_us + n * T;
        // The line is the earliest arrivals: an earlier frame moves it there; otherwise it
        // creeps up 10 us per frame (follows a path that got slower), the margin covering the
        // jitter above it.
        pc.line_us = ready < predicted + LINE_CREEP_US ? ready : predicted + LINE_CREEP_US; // never after ready
    }
    pc.last_ready_us = ready;
    pc.last_ts_ns = ts_ns;
    const uint64_t late = ready > pc.line_us ? ready - pc.line_us : 0;
    pc.lateness[pc.lateness_head] = late > 0xffffffffu ? 0xffffffffu : (uint32_t)late;
    pc.lateness_head = (pc.lateness_head + 1) % JITTER_SAMPLES;
    if (pc.lateness_count < JITTER_SAMPLES)
        pc.lateness_count++;
    if (pc.lateness_count >= 32 && pc.lateness_head % 16 == 0) { // every 16 frames
        // 99th percentile: the (count / 100 + 1)-th largest.
        const int k = pc.lateness_count / 100 + 1; // up to 11
        uint32_t top[12] = {};
        for (int i = 0; i < pc.lateness_count; i++) {
            uint32_t v = pc.lateness[i];
            for (int j = 0; j < k && v; j++)
                if (v > top[j]) {
                    const uint32_t t = top[j];
                    top[j] = v;
                    v = t;
                }
        }
        const uint64_t m = top[k - 1] + MARGIN_EXTRA_US;
        pc.margin_us = m > T ? T : m;
    }
    const uint64_t target = pc.line_us + pc.margin_us;
    const uint64_t next = pc.schedule_us + n * T;
    if (n == 0 || next > target + T / 2 || target > next + T / 2) {
        // Start, or more than half a period off (a frame counted for the wrong period):
        // back on the target at once.
        pc.schedule_us = target;
    } else {
        pc.schedule_us = target > next ? next + (target - next < SCHEDULE_UP_US ? target - next : SCHEDULE_UP_US)
                                       : next - (next - target < SCHEDULE_DOWN_US ? next - target : SCHEDULE_DOWN_US);
    }
    pc.hold_us = pc.schedule_us > ready ? pc.schedule_us - ready : 0;
    return pc.schedule_us;
}
static unsigned g_replaced; // decoded pictures replaced before their conversion (never shown)
static bool g_paused;       // video_set_paused: frames received are not decoded
static unsigned g_bench_pub_seq; // g_pub_seq when the bench began
static uint64_t g_bench_bytes;   // access unit bytes queued during the bench

static uint64_t now_us() { return sceKernelGetProcessTime(); }

// Video bench timing: histograms of 0.1 ms steps up to 100 ms (the last bucket takes the rest).
struct BenchHist {
    unsigned n;
    uint64_t sum_us, max_us;
    unsigned bucket[1001];
};
static struct {
    volatile bool on;
    BenchHist decode, convert, latency;
    unsigned replaced;
} g_bench;

static void bench_record(BenchHist *h, uint64_t us)
{
    if (!g_bench.on)
        return;
    h->n++;
    h->sum_us += us;
    if (us > h->max_us)
        h->max_us = us;
    h->bucket[us / 100 < 1000 ? us / 100 : 1000]++;
}

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
        sceKernelReleaseDirectMemory(phys, size);
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
// STREAM: the destination is an eye buffer, 16-byte aligned, written with non-temporal
// stores (the GPU reads it; plain stores would first read every line into the cache).
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
template <int ROWS, bool STREAM = false>
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
            if (STREAM) {
                _mm_stream_si128((__m128i *)(d + x), _mm_unpacklo_epi16(bg, ra));
                _mm_stream_si128((__m128i *)(d + x + 4), _mm_unpackhi_epi16(bg, ra));
            } else {
                _mm_storeu_si128((__m128i *)(d + x), _mm_unpacklo_epi16(bg, ra));
                _mm_storeu_si128((__m128i *)(d + x + 4), _mm_unpackhi_epi16(bg, ra));
            }
        }
    }
}

static void convert(const ConvertJob &j)
{
    if (j.w % 8 || j.x0 % 2 || ((uintptr_t)j.dst & 15) || j.dst_pitch % 4) {
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
            convert_span<2, true>(k, y0, y0 + j.pitch, uvp, d0, d0 + j.dst_pitch, j.w);
        else
            convert_span<1, true>(k, y0, y0, uvp, d0, d0, j.w);
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
    uint64_t col_weights[2][EYE_MAX_W]; // bytes (64 - w, w) x4, for _mm_maddubs_epi16
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
            l.col_weights[e][x] = 0x0001000100010001ull * (uint64_t)((wt << 8) | (64 - wt));
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
    uint32_t width; // eye width, rounded up to 4
};

// Each eye is converted in two bands: by the conversion thread and three helpers. With the
// decoder at depth 3, 4 jobs take 3.7 ms per frame, 6 jobs 4.5 ms (a core shared with the
// decoder's threads), 8 jobs 3.7 ms with more threads (video bench, 2026-09-29).
// Up to 8 jobs (the video bench tries other counts).
static const int CONVERT_JOBS = 4, CONVERT_JOBS_MAX = 8;
static int g_convert_jobs = CONVERT_JOBS;
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
// Per job: two converted source rows, a blended row and the expanded output row.
static uint32_t g_row_buf[CONVERT_JOBS_MAX][4][EYE_MAX_W + 32] __attribute__((aligned(64)));

// (a * (64 - w) + b * w + 32) >> 6 per channel: bytes of a and b interleaved, then one
// unsigned x signed byte multiply-add per pair.
static void blend_rows(const uint32_t *a, const uint32_t *b, int w, uint32_t *out, uint32_t n)
{
    const __m128i round = _mm_set1_epi16(32), wv = _mm_set1_epi16((short)((w << 8) | (64 - w)));
    for (uint32_t i = 0; i < n; i += 4) {
        __m128i pa = _mm_loadu_si128((const __m128i *)(a + i)), pb = _mm_loadu_si128((const __m128i *)(b + i));
        __m128i lo = _mm_maddubs_epi16(_mm_unpacklo_epi8(pa, pb), wv);
        __m128i hi = _mm_maddubs_epi16(_mm_unpackhi_epi8(pa, pb), wv);
        lo = _mm_srli_epi16(_mm_add_epi16(lo, round), 6);
        hi = _mm_srli_epi16(_mm_add_epi16(hi, round), 6);
        _mm_storeu_si128((__m128i *)(out + i), _mm_packus_epi16(lo, hi));
    }
}

// Expanded row into dst (a cached buffer; stream_row writes it out).
static void expand_row(const uint32_t *src, uint32_t *dst, int eye)
{
    const FfeLut &l = g_lut;
    const __m128i round = _mm_set1_epi16(32);
    // Each output pixel blends a source pair (p, p + 1): interleave their channels.
    const __m128i pairs = _mm_setr_epi8(0, 4, 1, 5, 2, 6, 3, 7, 8, 12, 9, 13, 10, 14, 11, 15);
    for (int r = 0; r < l.nruns[eye]; r++) {
        const ColumnRun &run = l.runs[eye][r];
        if (run.copy) {
            memcpy(dst + run.x0, src + l.col[eye][run.x0], (run.x1 - run.x0) * 4);
            continue;
        }
        const int32_t *col = l.col[eye];
        const uint64_t *wt = l.col_weights[eye];
        uint32_t x = run.x0;
        for (; x + 2 <= run.x1; x += 2) { // two output pixels
            __m128i p = _mm_unpacklo_epi64(_mm_loadl_epi64((const __m128i *)(src + col[x])),
                                           _mm_loadl_epi64((const __m128i *)(src + col[x + 1])));
            __m128i s = _mm_maddubs_epi16(_mm_shuffle_epi8(p, pairs), _mm_loadu_si128((const __m128i *)(wt + x)));
            s = _mm_srli_epi16(_mm_add_epi16(s, round), 6);
            _mm_storel_epi64((__m128i *)(dst + x), _mm_packus_epi16(s, s));
        }
        if (x < run.x1) {
            __m128i p = _mm_shuffle_epi8(_mm_loadl_epi64((const __m128i *)(src + col[x])), pairs);
            __m128i s = _mm_maddubs_epi16(p, _mm_loadl_epi64((const __m128i *)(wt + x)));
            s = _mm_srli_epi16(_mm_add_epi16(s, round), 6);
            dst[x] = (uint32_t)_mm_cvtsi128_si32(_mm_packus_epi16(s, s));
        }
    }
}

// Cached row out to an eye buffer row (both 16-byte aligned) with non-temporal stores:
// the eye buffers are only read by the GPU, and plain stores first read every line into
// the cache, doubling the memory traffic of the conversion. n is rounded up to 4 pixels
// (the pitch has room).
static void stream_row(uint32_t *dst, const uint32_t *src, uint32_t n)
{
    for (uint32_t i = 0; i < n; i += 4)
        _mm_stream_si128((__m128i *)(dst + i), _mm_load_si128((const __m128i *)(src + i)));
}

// Fade to black between the lobby and the stream (video_set_brightness): 0..64, applied by
// the conversion jobs to every frame converted while it is below 64 (nothing to do otherwise).
static volatile int g_brightness = 64;

// Pixels times level / 64, alpha kept. n is a multiple of 4.
static void darken_row(uint32_t *p, uint32_t n, int level)
{
    const __m128i zero = _mm_setzero_si128(), k = _mm_set1_epi16((short)level);
    const __m128i rgb = _mm_set1_epi32(0x00ffffff), alpha = _mm_set1_epi32((int)0x80000000);
    for (uint32_t i = 0; i < n; i += 4) {
        const __m128i v = _mm_loadu_si128((const __m128i *)(p + i));
        const __m128i lo = _mm_srli_epi16(_mm_mullo_epi16(_mm_unpacklo_epi8(v, zero), k), 6);
        const __m128i hi = _mm_srli_epi16(_mm_mullo_epi16(_mm_unpackhi_epi8(v, zero), k), 6);
        _mm_storeu_si128((__m128i *)(p + i), _mm_or_si128(_mm_and_si128(_mm_packus_epi16(lo, hi), rgb), alpha));
    }
}

// Rows of the compressed eye are converted to BGRA once each (two are cached), blended
// vertically when needed, then expanded horizontally and written out to the eye buffer.
static void ffe_band(const FfeJob &j, int slot, int level)
{
    const FfeLut &l = g_lut;
    Coeffs k;
    make_coeffs(k, j.full_range);
    uint32_t *cache[2] = {g_row_buf[slot][0], g_row_buf[slot][1]};
    int cached[2] = {-1, -1};
    uint32_t *blend = g_row_buf[slot][2], *out = g_row_buf[slot][3];
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
        expand_row(src, out, j.eye);
        if (level < 64)
            darken_row(out, j.width, level);
        stream_row(j.dst + (size_t)y * j.dst_pitch, out, j.width);
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
static Job g_jobs[CONVERT_JOBS_MAX];
static unsigned g_job_gen, g_jobs_done;
static int g_active_jobs = CONVERT_JOBS; // jobs of the current frame
static int g_job_brightness = 64;        // brightness of the current frame (g_brightness when it started)

static void run_job(int i)
{
    const int level = g_job_brightness;
    if (g_jobs[i].ffe) {
        ffe_band(g_jobs[i].fov, i, level);
    } else {
        convert(g_jobs[i].conv);
        if (level < 64) { // only during a fade: the rows just written are read back
            _mm_sfence();
            const ConvertJob &c = g_jobs[i].conv;
            for (uint32_t r = 0; r < c.h; r++)
                darken_row(c.dst + (size_t)r * c.dst_pitch, (c.w + 3) & ~3u, level);
        }
    }
    _mm_sfence(); // non-temporal stores visible before the frame is published
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
        const bool mine = index < g_active_jobs;
        pthread_mutex_unlock(&g_job_lock);
        if (!mine)
            continue;
        run_job(index);
        pthread_mutex_lock(&g_job_lock);
        g_jobs_done++;
        pthread_cond_broadcast(&g_job_cond);
        pthread_mutex_unlock(&g_job_lock);
    }
    return nullptr;
}

// ---------------------------------------------------------------------------------------
// Performance overlay: text lines drawn into both eyes of each converted frame, below the
// centre of the view, straight ahead at OVERLAY_DEPTH_M (each eye offset for its own
// position), on a darkened box. Drawn in the eye buffers, it follows the head like the
// picture it is part of.

#include "font_data.h"

static const int OVERLAY_LINES = 4, OVERLAY_CHARS = 64;
static const float OVERLAY_DEPTH_M = 1.5f, OVERLAY_HALF_IPD_M = 0.0315f;
static const float OVERLAY_TAN_Y = -0.30f; // box centre, as the tangent of the angle below the view centre
static pthread_mutex_t g_overlay_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    int count;
    char text[OVERLAY_LINES][OVERLAY_CHARS];
    uint32_t rgb[OVERLAY_LINES];
    float tan[2][4]; // per eye: left, right, up, down
} g_overlay;

void video_set_overlay(const char *const *lines, const uint32_t *rgb, int count, const float tangents[2][4])
{
    pthread_mutex_lock(&g_overlay_lock);
    g_overlay.count = count < 0 ? 0 : count > OVERLAY_LINES ? OVERLAY_LINES : count;
    for (int i = 0; i < g_overlay.count; i++) {
        strncpy(g_overlay.text[i], lines[i], OVERLAY_CHARS - 1);
        g_overlay.text[i][OVERLAY_CHARS - 1] = 0;
        g_overlay.rgb[i] = rgb ? rgb[i] : 0xffffff;
    }
    if (tangents)
        memcpy(g_overlay.tan, tangents, sizeof(g_overlay.tan));
    pthread_mutex_unlock(&g_overlay_lock);
}

static void draw_overlay(int set)
{
    pthread_mutex_lock(&g_overlay_lock);
    const int n = g_overlay.count;
    if (n == 0 || g_overlay.tan[0][0] + g_overlay.tan[0][1] <= 0.0f || g_overlay.tan[0][2] + g_overlay.tan[0][3] <= 0.0f ||
        g_overlay.tan[1][0] + g_overlay.tan[1][1] <= 0.0f || g_overlay.tan[1][2] + g_overlay.tan[1][3] <= 0.0f) {
        pthread_mutex_unlock(&g_overlay_lock);
        return;
    }
    static char text[OVERLAY_LINES][OVERLAY_CHARS];
    static uint32_t rgb[OVERLAY_LINES];
    float tan[2][4];
    memcpy(text, g_overlay.text, sizeof(text));
    memcpy(rgb, g_overlay.rgb, sizeof(rgb));
    memcpy(tan, g_overlay.tan, sizeof(tan));
    pthread_mutex_unlock(&g_overlay_lock);

    const int W = (int)g_eye_w, H = (int)g_eye_h, pitch = (int)g_eye_pitch;
    // Glyphs doubled on large eye buffers (above 1600 pixels wide), so the text keeps its size.
    const int scale = W > 1600 ? 2 : 1, lh = (FONT_H + 4) * scale, pad = 10 * scale;
    int chars = 0;
    for (int i = 0; i < n; i++) {
        const int l = (int)strlen(text[i]);
        chars = l > chars ? l : chars;
    }
    const int bw = chars * FONT_W * scale + 2 * pad, bh = n * lh + 2 * pad - 4 * scale;
    for (int e = 0; e < 2; e++) {
        const float *t = tan[e];
        const float tx = (e == 0 ? OVERLAY_HALF_IPD_M : -OVERLAY_HALF_IPD_M) / OVERLAY_DEPTH_M;
        const int cx = (int)((t[0] + tx) / (t[0] + t[1]) * W), cy = (int)((t[2] - OVERLAY_TAN_Y) / (t[2] + t[3]) * H);
        const int x0 = cx - bw / 2, y0 = cy - bh / 2;
        uint32_t *img = g_eye_mem[set][e];
        for (int y = y0 < 0 ? 0 : y0; y < y0 + bh && y < H; y++) {
            uint32_t *row = img + (size_t)y * pitch;
            for (int x = x0 < 0 ? 0 : x0; x < x0 + bw && x < W; x++)
                row[x] = (row[x] & 0xff000000) | ((row[x] >> 2) & 0x3f3f3f); // a quarter of the brightness
        }
        for (int i = 0; i < n; i++) {
            const uint32_t px = 0x80000000 | rgb[i]; // alpha as the conversion writes it
            int x = x0 + pad;
            for (const char *c = text[i]; *c; c++, x += FONT_W * scale) {
                const unsigned ch = (unsigned char)*c < FONT_FIRST || (unsigned char)*c > FONT_LAST ? '?' : (unsigned char)*c;
                const uint16_t *glyph = font_data[ch - FONT_FIRST];
                for (int gy = 0; gy < FONT_H * scale; gy++) {
                    const int y = y0 + pad + i * lh + gy;
                    if (y < 0 || y >= H)
                        continue;
                    const uint16_t bits = glyph[gy / scale];
                    uint32_t *row = img + (size_t)y * pitch;
                    for (int gx = 0; gx < FONT_W * scale; gx++)
                        if ((bits & (1 << (FONT_W - 1 - gx / scale))) && x + gx >= 0 && x + gx < W)
                            row[x + gx] = px;
                }
            }
        }
    }
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
    // No clearing: every set is fully written before it is shown (and one may be on screen).
    for (int s = 0; s < EYE_SETS; s++)
        for (int e = 0; e < 2; e++)
            gnm_texture_linear_bgra(&g_eye_tex[s][e], g_eye_mem[s][e], w, h, pitch);
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
static uint64_t g_ts_fifo[16], g_rx_fifo[16]; // timestamp, received time
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
    // Allocated once: a failed attempt keeps it for the next one.
    static void *queue_mem;
    if (!queue_mem)
        queue_mem = alloc_direct(mem.cpu_gpu_memory_size, 0x10000, MEM_ONION, "compute queue");
    mem.cpu_gpu_memory = queue_mem;
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

static void delete_decoder()
{
    if (!g_decoder)
        return;
    p_delete(g_decoder);
    g_decoder = nullptr;
}

static bool create_decoder(uint32_t width, uint32_t height, uint32_t codec)
{
    delete_decoder();
    if (!create_compute_queue())
        return false;
    Vdec2ConfigInfo cfg{};
    cfg.this_size = sizeof(cfg);
    // resource_type 1: the decoder the system offers apps, software (libSceVdecSavc2 on the
    // CPU + GPU compute; reference/decomp/vdeccore_*.c). The other types it accepts, 0xb6c8
    // and the PS4 Pro's 0x12384 (Pro mode apps only), run the same software core and failed
    // to create (2026-09-30). HEVC: codec 0xee049, profile 1 (Main), level_idc 153 (5.1);
    // it decodes, but slower than H.264.
    cfg.resource_type = 1;
    cfg.codec_type = codec == 1 ? 0xee049 : 1;
    cfg.profile = codec == 1 ? 1 : 100; // H.264 High covers Main and Baseline streams
    cfg.max_level = codec == 1 ? 153 : 52;
    cfg.max_frame_width = (int32_t)((width + 15) & ~15u);
    cfg.max_frame_height = (int32_t)((height + 15) & ~15u);
    cfg.max_dpb_frame_count = 16;
    cfg.decode_pipeline_depth = g_use_depth;
    cfg.compute_queue = g_compute_queue;
    cfg.cpu_affinity_mask = 0x3f;
    cfg.cpu_thread_priority = 256; // highest allowed: decoding is on the latency path
    cfg.optimize_progressive = 1;
    cfg.check_memory_type = 0;
    Vdec2MemoryInfo mem{};
    mem.this_size = sizeof(mem);
    int rc = p_query_decoder(&cfg, &mem);
    LOG("video: QueryDecoderMemoryInfo %s depth %u %dx%d -> 0x%08x cpu 0x%llx gpu 0x%llx cpu_gpu 0x%llx frame 0x%llx "
        "align 0x%x",
        codec == 1 ? "HEVC" : "H.264", cfg.decode_pipeline_depth, cfg.max_frame_width, cfg.max_frame_height,
        (unsigned)rc, (unsigned long long)mem.cpu_memory_size,
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
    g_dec_depth = g_use_depth;
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
    if (rc == 0 && g_ts_count < 16) {
        const int k = (g_ts_head + g_ts_count++) % 16;
        g_ts_fifo[k] = s.timestamp_ns;
        g_rx_fifo[k] = s.received_us;
    }
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
    bench_record(&g_bench.decode, t1 - t0);
    if (c0 && c1 >= c0)
        g_stats.decode_cpu_us_avg = (g_stats.decode_cpu_us_avg * 15 + (c1 - c0)) / 16;
    pthread_mutex_unlock(&g_lock);
    if (!out.is_valid)
        return;
    // The picture is expected in the frame buffer given to this call.
    if (out.frame_buffer != g_frame_buffers[fb_index]) {
        static unsigned mismatch_logged;
        if (mismatch_logged++ < 20)
            LOG("video: picture in frame buffer %p, %p was given", out.frame_buffer, g_frame_buffers[fb_index]);
        for (int i = 0; i < FRAME_BUFFERS; i++)
            if (out.frame_buffer == g_frame_buffers[i])
                fb_index = i;
    }
    uint64_t ts = s.timestamp_ns, rx = s.received_us;
    if (g_ts_count > 0) {
        ts = g_ts_fifo[g_ts_head];
        rx = g_rx_fifo[g_ts_head];
        g_ts_head = (g_ts_head + 1) % 16;
        g_ts_count--;
    }
    // Hand the picture to the conversion thread; an older one it has not started is
    // replaced (its frame buffer becomes free again). Decoding and conversion overlap.
    pthread_mutex_lock(&g_dec_lock);
    if (g_has_pending) { // the conversion is behind: that picture is never shown
        g_bench.replaced++;
        g_replaced++;
    }
    g_pending = Decoded{fb_index, out.frame_width, out.frame_height,
                        out.frame_pitch_in_bytes ? out.frame_pitch_in_bytes : out.frame_pitch, ts, rx, t1, s.epoch};
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
        const bool stale = d.epoch != g_epoch;
        pthread_mutex_unlock(&g_lock);
        if (stale) { // decoded before a reset or a pause
            pthread_mutex_lock(&g_dec_lock);
            g_converting_fb = -1;
            pthread_mutex_unlock(&g_dec_lock);
            continue;
        }
        if (ffe && (d.width < ax.compressed * 2 || d.height < ay.compressed)) {
            static bool warned;
            if (!warned)
                LOG("video: decoded %ux%u is smaller than the foveated %ux%u frame, shown without expansion",
                    d.width, d.height, ax.compressed * 2, ay.compressed);
            warned = true;
            ffe = false;
        }
        if (ffe && (eye_w > EYE_MAX_W || eye_h > EYE_MAX_H)) {
            // Too large for the LUT and the eye buffers: alloc_eye_buffers refuses it below.
        } else if (ffe) {
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
            for (w = 0; w < EYE_SETS; w++) {
                bool used = w == g_displayed || w == g_prev_displayed;
                for (int i = 0; i < g_ready_count; i++)
                    used = used || g_ready[i].set == w;
                if (!used)
                    break;
            }
            pthread_mutex_unlock(&g_pub_lock);
            const uint8_t *y = (const uint8_t *)g_frame_buffers[d.fb];
            const uint8_t *uv = y + (size_t)d.pitch * d.height;
            const int jobs = g_convert_jobs, bands = jobs / 2;
            // Each eye in horizontal bands, one per job (even split rows keep chroma pairs).
            for (int e = 0; e < 2; e++)
                for (int band = 0; band < bands; band++) {
                    Job &job = g_jobs[e * bands + band];
                    auto split = [&](int b) -> uint32_t {
                        uint32_t r = eye_h * (uint32_t)b / bands;
                        return b == bands ? eye_h : ffe ? r : r & ~1u;
                    };
                    uint32_t r0 = split(band), r1 = split(band + 1);
                    job.ffe = ffe;
                    if (ffe)
                        job.fov = FfeJob{y, uv, d.pitch, e ? ax.compressed : 0, e, r0, r1,
                                         g_eye_mem[w][e], g_eye_pitch, full_range, (eye_w + 3) & ~3u};
                    else
                        job.conv = ConvertJob{y + (size_t)r0 * d.pitch, uv + (size_t)(r0 / 2) * d.pitch, d.pitch,
                                              e ? eye_w : 0, eye_w & ~1u, r1 - r0,
                                              g_eye_mem[w][e] + (size_t)r0 * g_eye_pitch, g_eye_pitch, full_range};
                }
            uint64_t t0 = now_us();
            pthread_mutex_lock(&g_job_lock);
            g_jobs_done = 0;
            g_active_jobs = jobs;
            g_job_brightness = g_brightness;
            g_job_gen++;
            pthread_cond_broadcast(&g_job_cond);
            pthread_mutex_unlock(&g_job_lock);
            run_job(0);
            pthread_mutex_lock(&g_job_lock);
            while (g_jobs_done < (unsigned)jobs - 1)
                pthread_cond_wait(&g_job_cond, &g_job_lock);
            pthread_mutex_unlock(&g_job_lock);
            if (g_job_brightness == 64) // not over a fade
                draw_overlay(w);
            uint64_t t1 = now_us();
            bench_record(&g_bench.convert, t1 - t0);
            if (d.received_us)
                bench_record(&g_bench.latency, t1 - d.received_us);

            pthread_mutex_lock(&g_pub_lock);
            if (d.epoch != g_pub_epoch) { // reset or paused during the conversion
                pthread_mutex_unlock(&g_pub_lock);
                pthread_mutex_lock(&g_dec_lock);
                g_converting_fb = -1;
                pthread_mutex_unlock(&g_dec_lock);
                continue;
            }
            if (g_ready_count == READY_MAX) { // the display is behind: the oldest is never shown
                memmove(&g_ready[0], &g_ready[1], sizeof(Ready) * (READY_MAX - 1));
                g_ready_count--;
                g_ready_overflow++;
            }
            g_ready[g_ready_count++] = Ready{w, d.timestamp_ns, t1, d.received_us, d.picture_us,
                                             pacing_due(t1, d.timestamp_ns)};
            g_stream_valid = true;
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
        const unsigned epoch = g_epoch;
        unsigned gen = g_config_gen;
        uint32_t codec = g_codec, fw = g_frame_w, fh = g_frame_h;
        static uint8_t config[CONFIG_MAX];
        size_t config_len = s.idr ? g_config_len : 0;
        memcpy(config, g_config, config_len);
        g_use_depth = g_bench.on ? g_decode_depth : g_stream_period_us > 14000 ? DECODE_DEPTH_60FPS : DECODE_DEPTH;
        const uint32_t use_depth = g_use_depth;
        pthread_mutex_unlock(&g_lock);

        // A new epoch (reset, pause, resume): the pictures still inside the decoder and the
        // one waiting for conversion are from before; slots queued before are skipped.
        static unsigned dec_epoch;
        if (epoch != dec_epoch) {
            dec_epoch = epoch;
            if (g_decoder)
                p_reset(g_decoder);
            ts_fifo_clear();
            pthread_mutex_lock(&g_dec_lock);
            g_has_pending = false;
            pthread_mutex_unlock(&g_dec_lock);
        }
        bool ok = s.epoch == epoch;
        // A failed creation is retried only with a new DecoderConfig or pipeline depth, not
        // at every frame (each attempt logs and may allocate).
        static bool create_failed;
        static unsigned failed_gen;
        static uint32_t failed_depth;
        const bool blocked = create_failed && gen == failed_gen && use_depth == failed_depth;
        if (ok && !blocked && (!g_decoder || gen != g_decoder_gen || g_dec_depth != use_depth)) {
            // ALVR codec 0 = H.264, 1 = HEVC.
            if (codec > 1) {
                static bool warned;
                if (!warned)
                    LOG("video: codec %u is not supported, set the streamer to H264 or HEVC", codec);
                warned = true;
                ok = false;
            } else {
                ok = create_decoder(fw, fh, codec);
                create_failed = !ok;
                failed_gen = gen;
                failed_depth = use_depth;
                if (!ok)
                    LOG("video: decoder creation failed (retried with the next DecoderConfig)");
                g_decoder_gen = gen;
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
    for (int i = 1; i < CONVERT_JOBS_MAX; i++)
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
    if (len > CONFIG_MAX) {
        LOG("video: decoder config too large (%zu bytes)", len);
        len = 0;
    }
    bool changed = codec != g_codec || len != g_config_len || memcmp(config, g_config, len) != 0;
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
    // Arrival regularity: gaps well above the usual frame interval, frames bunched together.
    const uint64_t t = now_us();
    static uint64_t last_arrival_us, interval_avg_us;
    if (last_arrival_us && t > last_arrival_us && t - last_arrival_us < 500000) {
        const uint64_t gap = t - last_arrival_us;
        if (interval_avg_us && gap > interval_avg_us * 18 / 10)
            g_stats.arrival_gaps++;
        if (gap < 2000)
            g_stats.arrival_bunched++;
        if (gap > g_stats.arrival_gap_max_us)
            g_stats.arrival_gap_max_us = gap;
        interval_avg_us = interval_avg_us ? (interval_avg_us * 63 + gap) / 64 : gap;
    }
    last_arrival_us = t;
    g_stats.bytes_total += len;
    if (g_paused) { // lobby while streaming: nothing is decoded
        pthread_mutex_unlock(&g_lock);
        return;
    }
    if (len > SLOT_SIZE - CONFIG_MAX) { // too large for a slot: the next frames lack it
        g_stats.dropped++;
        if (!g_bench.on)
            request_idr_locked();
        pthread_mutex_unlock(&g_lock);
        return;
    }
    if (is_idr)
        g_wait_idr = false;
    if (g_wait_idr || g_config_len == 0) {
        g_stats.dropped++;
        pthread_mutex_unlock(&g_lock);
        return;
    }
    // A full queue, or a backlog that does not go away, would only add latency (up to 190 ms
    // was seen in busy Beat Saber scenes before the decoder pipeline was deepened): drop the
    // frame and restart from an IDR.
    static uint64_t backlog_since;
    if (g_count < BACKLOG_FRAMES)
        backlog_since = 0;
    else if (!backlog_since)
        backlog_since = t;
    if (g_count >= MAX_QUEUED || (backlog_since && t - backlog_since >= BACKLOG_US)) {
        g_stats.dropped++;
        backlog_since = 0;
        if (!g_bench.on) // a bench clip has no IDR to come back to
            request_idr_locked();
        pthread_mutex_unlock(&g_lock);
        return;
    }
    Slot &s = g_slots[(g_head + g_count) % QUEUE_SLOTS];
    memcpy(s.mem + CONFIG_MAX, data, len);
    s.len = len;
    s.timestamp_ns = timestamp_ns;
    s.received_us = t;
    s.idr = is_idr;
    s.epoch = g_epoch;
    g_count++;
    g_stats.bytes_avg = (g_stats.bytes_avg * 15 + len) / 16;
    if (g_bench.on)
        g_bench_bytes += len;
    if ((unsigned)g_count > g_stats.queue_max)
        g_stats.queue_max = g_count;
    pthread_cond_signal(&g_cond);
    pthread_mutex_unlock(&g_lock);
}

void video_bench_set_options(const VideoBenchOptions *o)
{
    uint32_t depth = o ? o->decode_depth : DECODE_DEPTH;
    int jobs = o ? o->convert_jobs : CONVERT_JOBS;
    g_decode_depth = depth >= 1 && depth <= 8 ? depth : DECODE_DEPTH;
    g_convert_jobs = jobs >= 2 && jobs <= CONVERT_JOBS_MAX && jobs % 2 == 0 ? jobs : CONVERT_JOBS;
    LOG("video: decoder depth %u, %d conversion jobs", g_decode_depth, g_convert_jobs);
}

void video_bench_begin()
{
    pthread_mutex_lock(&g_lock);
    memset(&g_stats, 0, sizeof(g_stats));
    pthread_mutex_unlock(&g_lock);
    memset((void *)&g_bench, 0, sizeof(g_bench));
    pthread_mutex_lock(&g_lock);
    g_bench_bytes = 0;
    pthread_mutex_unlock(&g_lock);
    pthread_mutex_lock(&g_pub_lock);
    g_bench_pub_seq = g_pub_seq;
    pthread_mutex_unlock(&g_pub_lock);
    g_bench.on = true;
}

static VideoTimes bench_times(const BenchHist &h)
{
    VideoTimes t{};
    t.n = h.n;
    if (!h.n)
        return t;
    t.avg = h.sum_us / 1000.0 / h.n;
    t.max = h.max_us / 1000.0;
    const double q[3] = {0.5, 0.9, 0.99};
    double *out[3] = {&t.p50, &t.p90, &t.p99};
    for (int k = 0; k < 3; k++) {
        unsigned target = (unsigned)(q[k] * h.n), acc = 0;
        for (int b = 0; b <= 1000; b++) {
            acc += h.bucket[b];
            if (acc > target) {
                *out[k] = (b + 0.5) / 10.0;
                break;
            }
        }
    }
    return t;
}

void video_bench_end(VideoBenchResult *r)
{
    g_bench.on = false;
    memset(r, 0, sizeof(*r));
    pthread_mutex_lock(&g_lock);
    r->received = g_stats.received;
    r->decoded = g_stats.decoded;
    r->dropped = g_stats.dropped;
    r->errors = g_stats.errors;
    r->bytes = g_bench_bytes;
    pthread_mutex_unlock(&g_lock);
    pthread_mutex_lock(&g_pub_lock);
    r->published = g_pub_seq - g_bench_pub_seq;
    pthread_mutex_unlock(&g_pub_lock);
    r->replaced = g_bench.replaced;
    r->decode = bench_times(g_bench.decode);
    r->convert = bench_times(g_bench.convert);
    r->latency = bench_times(g_bench.latency);
}

unsigned video_bench_published()
{
    pthread_mutex_lock(&g_pub_lock);
    unsigned n = g_pub_seq - g_bench_pub_seq;
    pthread_mutex_unlock(&g_pub_lock);
    return n;
}

int video_queued()
{
    pthread_mutex_lock(&g_lock);
    int n = g_count;
    pthread_mutex_unlock(&g_lock);
    return n;
}

void video_packet_loss()
{
    pthread_mutex_lock(&g_lock);
    g_stats.lost++;
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
    // Nothing while paused (lobby): the request is made when the stream resumes.
    bool want = !g_paused && ((g_want_idr && since >= 100000) || ((g_wait_idr || !g_config_len) && since >= 500000));
    if (want) {
        g_want_idr = false;
        g_last_idr_request_us = t;
    }
    pthread_mutex_unlock(&g_lock);
    return want;
}

void video_reset()
{
    // Queued frames are left to the decode thread (it owns the head slot): from an older
    // epoch, they are skipped.
    pthread_mutex_lock(&g_lock);
    g_wait_idr = true;
    g_want_idr = false;
    g_config_len = 0;
    g_codec = 0xffffffff;
    const unsigned epoch = ++g_epoch;
    pthread_mutex_unlock(&g_lock);
    pthread_mutex_lock(&g_pub_lock);
    g_pub_epoch = epoch;
    g_stream_valid = false;
    g_disp_valid = false;
    g_ready_count = 0;
    g_pacing.line_valid = false;
    pthread_mutex_unlock(&g_pub_lock);
}

bool video_next(VideoFrame *out, bool take, bool paced)
{
    pthread_mutex_lock(&g_pub_lock);
    int pick = -1;
    if (take && paced) {
        // The newest frame due more than the hysteresis before now (older ones are skipped),
        // else the oldest one if due by then (see PACING_HYSTERESIS_US).
        const uint64_t t = now_us();
        for (int i = 0; i < g_ready_count; i++)
            if (g_ready[i].due_us + PACING_HYSTERESIS_US <= t)
                pick = i;
        if (pick < 0 && g_ready_count > 0 && g_ready[0].due_us <= t + PACING_HYSTERESIS_US)
            pick = 0;
        // Safety net: never let frames pile up waiting for a due time that does not come (a
        // full FIFO, or a frame held more than 3 periods): the newest is shown and the
        // schedule starts again from the next frame.
        if (g_ready_count > 0 && pick < 0 &&
            (g_ready_count == READY_MAX || t - g_ready[0].decoded_us > 3 * g_pacing.period_us)) {
            pick = g_ready_count - 1;
            g_pacing.line_valid = false;
            g_pacing.resyncs++;
        }
        if (pick >= 0 && g_pacing.last_decision_us && g_ready[pick].due_us <= g_pacing.last_decision_us &&
            t - g_pacing.last_decision_us < 3 * g_pacing.period_us)
            g_pacing.late++; // it was due at the previous decision but not converted yet
        g_pacing.last_decision_us = t;
    } else if (take && g_ready_count > 0) {
        pick = g_ready_count - 1; // unpaced: the newest
    }
    if (pick >= 0) {
        g_ready_trimmed += pick;
        const Ready r = g_ready[pick];
        memmove(&g_ready[0], &g_ready[pick + 1], sizeof(Ready) * (READY_MAX - pick - 1));
        g_ready_count -= pick + 1;
        g_prev_displayed = g_displayed;
        g_displayed = r.set;
        g_disp_valid = true;
        g_disp_ts = r.timestamp_ns;
        g_disp_decoded_us = r.decoded_us;
        g_disp_received_us = r.received_us;
        g_disp_picture_us = r.picture_us;
        g_disp_seq++;
    }
    bool ok = g_stream_valid && g_disp_valid && g_displayed >= 0;
    if (ok) {
        out->eye[0] = &g_eye_tex[g_displayed][0];
        out->eye[1] = &g_eye_tex[g_displayed][1];
        out->timestamp_ns = g_disp_ts;
        out->decoded_us = g_disp_decoded_us;
        out->received_us = g_disp_received_us;
        out->picture_us = g_disp_picture_us;
        out->seq = g_disp_seq;
        out->waiting = g_ready_count;
    }
    pthread_mutex_unlock(&g_pub_lock);
    return ok;
}

void video_pacing_stats(VideoPacingStats *out)
{
    pthread_mutex_lock(&g_pub_lock);
    out->overflow = g_ready_overflow;
    out->trimmed = g_ready_trimmed;
    out->late = g_pacing.late;
    out->margin_us = (unsigned)g_pacing.margin_us;
    out->hold_us = (unsigned)g_pacing.hold_us;
    out->resyncs = g_pacing.resyncs;
    pthread_mutex_unlock(&g_pub_lock);
    pthread_mutex_lock(&g_dec_lock);
    out->replaced = g_replaced;
    pthread_mutex_unlock(&g_dec_lock);
}

void video_set_paused(bool paused)
{
    pthread_mutex_lock(&g_lock);
    const bool changed = paused != g_paused;
    g_paused = paused;
    if (changed && !paused) {
        // Frames were skipped: decoding restarts from a fresh IDR frame.
        g_wait_idr = true;
        g_want_idr = true;
        g_last_idr_request_us = 0;
    }
    const unsigned epoch = changed ? ++g_epoch : g_epoch;
    pthread_mutex_unlock(&g_lock);
    if (!changed)
        return;
    // The pictures decoded before are stale: none is shown again.
    pthread_mutex_lock(&g_pub_lock);
    g_pub_epoch = epoch;
    g_stream_valid = false;
    g_disp_valid = false;
    g_ready_count = 0;
    g_pacing.line_valid = false;
    pthread_mutex_unlock(&g_pub_lock);
    LOG("video: %s", paused ? "paused (lobby), frames are no longer decoded" : "resumed, IDR frame requested");
}

void video_set_brightness(float b)
{
    g_brightness = b <= 0.0f ? 0 : b >= 1.0f ? 64 : (int)(b * 64.0f + 0.5f);
}

bool video_frame_ready()
{
    pthread_mutex_lock(&g_pub_lock);
    const bool ready = g_ready_count > 0;
    pthread_mutex_unlock(&g_pub_lock);
    return ready;
}

void video_set_frame_period(uint32_t period_us)
{
    static uint32_t last; // called by the main loop at every iteration
    if (period_us < 4000 || period_us > 40000 || period_us == last)
        return;
    last = period_us;
    pthread_mutex_lock(&g_pub_lock);
    g_pacing.period_us = period_us;
    pthread_mutex_unlock(&g_pub_lock);
    pthread_mutex_lock(&g_lock);
    g_stream_period_us = period_us;
    pthread_mutex_unlock(&g_lock);
}

void video_peek_stats(VideoStats *out)
{
    pthread_mutex_lock(&g_lock);
    *out = g_stats;
    pthread_mutex_unlock(&g_lock);
}

void video_get_stats(VideoStats *out)
{
    pthread_mutex_lock(&g_lock);
    *out = g_stats;
    g_stats.queue_max = 0;
    g_stats.arrival_gaps = g_stats.arrival_bunched = 0;
    g_stats.arrival_gap_max_us = 0;
    pthread_mutex_unlock(&g_lock);
}

unsigned video_published_seq()
{
    pthread_mutex_lock(&g_pub_lock);
    unsigned s = g_pub_seq;
    pthread_mutex_unlock(&g_pub_lock);
    return s;
}

bool video_wait_new(unsigned after_seq, uint32_t timeout_us)
{
    uint64_t end = now_us() + timeout_us;
    for (;;) {
        pthread_mutex_lock(&g_pub_lock);
        bool fresh = g_stream_valid && g_pub_seq != after_seq;
        pthread_mutex_unlock(&g_pub_lock);
        if (fresh)
            return true;
        if (now_us() >= end)
            return false;
        sceKernelUsleep(500);
    }
}
