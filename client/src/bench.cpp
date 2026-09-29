#include "bench.h"

#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <orbis/libkernel.h>

#include "alvr_client.h"
#include "log.h"
#include "video.h"

#ifndef SO_NOSIGPIPE
#define SO_NOSIGPIPE 0x0800 // FreeBSD's (see alvr_client.cpp)
#endif

// Wire format (little endian), repeated for each test on one connection:
//   "ALVB", u32 header length, header JSON, u32 config length, SPS/PPS (Annex B),
//   u32 frame count, then per frame: u32 length | 0x80000000 for an IDR, access unit.
// Reply: u32 length, result JSON. The connection is closed on any error.
struct Header {
    char name[64];
    uint32_t view_w, view_h;
    bool full_range;
    bool ffe;
    FoveationSettings ffe_settings;
    double fps; // 0: as fast as the pipeline takes frames (queue kept at 2)
    int loops;
    int depth, jobs;
};

static const size_t CLIP_MAX = 160u << 20;
static uint8_t *g_clip;              // access units back to back
static const int FRAMES_MAX = 4096;
static uint32_t g_frame_off[FRAMES_MAX], g_frame_len[FRAMES_MAX];
static bool g_frame_idr[FRAMES_MAX];
static volatile bool g_active;

bool bench_active()
{
    return g_active;
}

static bool recv_all(int fd, void *buf, size_t n)
{
    uint8_t *p = (uint8_t *)buf;
    while (n) {
        ssize_t r = recv(fd, p, n, 0);
        if (r <= 0)
            return false;
        p += r;
        n -= (size_t)r;
    }
    return true;
}

static bool send_all(int fd, const void *buf, size_t n)
{
    const uint8_t *p = (const uint8_t *)buf;
    while (n) {
        ssize_t r = send(fd, p, n, 0);
        if (r <= 0)
            return false;
        p += r;
        n -= (size_t)r;
    }
    return true;
}

static bool send_json(int fd, const char *json)
{
    uint32_t n = (uint32_t)strlen(json);
    return send_all(fd, &n, 4) && send_all(fd, json, n);
}

static const char *json_value(const char *json, const char *key)
{
    char pat[48];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(json, pat);
    return p ? p + strlen(pat) : nullptr;
}

static double json_num(const char *json, const char *key, double def)
{
    const char *v = json_value(json, key);
    return v ? strtod(v, nullptr) : def;
}

static bool parse_header(const char *json, Header *h)
{
    memset(h, 0, sizeof(*h));
    if (const char *v = json_value(json, "name")) {
        if (*v == '"') {
            v++;
            size_t i = 0;
            while (v[i] && v[i] != '"' && i + 1 < sizeof(h->name)) {
                h->name[i] = v[i];
                i++;
            }
        }
    }
    h->view_w = (uint32_t)json_num(json, "view_w", 1248);
    h->view_h = (uint32_t)json_num(json, "view_h", 1376);
    h->full_range = json_num(json, "full_range", 1) != 0;
    h->fps = json_num(json, "fps", 90);
    h->loops = (int)json_num(json, "loops", 1);
    h->depth = (int)json_num(json, "depth", 2);
    h->jobs = (int)json_num(json, "jobs", 6);
    FoveationSettings &f = h->ffe_settings;
    f.center_size_x = (float)json_num(json, "ffe_center_x", 0);
    f.center_size_y = (float)json_num(json, "ffe_center_y", 0);
    f.center_shift_x = (float)json_num(json, "ffe_shift_x", 0);
    f.center_shift_y = (float)json_num(json, "ffe_shift_y", 0);
    f.edge_ratio_x = (float)json_num(json, "ffe_ratio_x", 0);
    f.edge_ratio_y = (float)json_num(json, "ffe_ratio_y", 0);
    h->ffe = f.edge_ratio_x > 1.0f || f.edge_ratio_y > 1.0f;
    if (h->loops < 1)
        h->loops = 1;
    return h->view_w && h->view_h && h->view_w <= 1536 && h->view_h <= 1728;
}

static int times_json(char *out, size_t n, const char *name, const VideoTimes &t)
{
    return snprintf(out, n, "\"%s\":{\"n\":%u,\"avg\":%.2f,\"p50\":%.2f,\"p90\":%.2f,\"p99\":%.2f,\"max\":%.2f}", name,
                    t.n, t.avg, t.p50, t.p90, t.p99, t.max);
}

// Plays the stored clip and fills the result JSON.
static void run_clip(const Header &h, const uint8_t *config, uint32_t config_len, int frames, char *out, size_t n)
{
    AlvrStatus st;
    alvr_get_status(&st);
    if (st.state == ALVR_STREAMING) {
        snprintf(out, n, "{\"name\":\"%s\",\"ok\":false,\"error\":\"a PC is streaming: close SteamVR first\"}", h.name);
        return;
    }
    LOG("bench: %s: %d frames x %d, %ux%u per eye%s, %.0f fps, depth %d, %d jobs", h.name, frames, h.loops, h.view_w,
        h.view_h, h.ffe ? " (foveated)" : "", h.fps, h.depth, h.jobs);
    VideoBenchOptions opt{(uint32_t)h.depth, h.jobs};
    video_bench_set_options(&opt);
    video_reset();
    video_set_stream(h.view_w, h.view_h, h.full_range, h.ffe ? &h.ffe_settings : nullptr);
    video_set_decoder_config(0, config, config_len);
    g_active = true;
    sceKernelUsleep(100000); // decoder recreation happens on the first frame
    video_bench_begin();
    const uint64_t period = h.fps > 0 ? (uint64_t)(1e6 / h.fps) : 0;
    const uint64_t t0 = sceKernelGetProcessTime();
    uint64_t next = t0;
    for (int loop = 0; loop < h.loops; loop++)
        for (int i = 0; i < frames; i++) {
            if (period) {
                uint64_t now = sceKernelGetProcessTime();
                if (next > now)
                    sceKernelUsleep((uint32_t)(next - now));
                next += period;
            } else {
                while (video_queued() >= 2)
                    sceKernelUsleep(200);
            }
            video_push_frame(sceKernelGetProcessTime() * 1000ull, g_frame_idr[i], g_clip + g_frame_off[i],
                             g_frame_len[i]);
        }
    const uint64_t t_pushed = sceKernelGetProcessTime();
    // Drain: until nothing is queued and no new picture came for 100 ms (1 s at most).
    unsigned last_pub = video_bench_published();
    uint64_t still_since = sceKernelGetProcessTime();
    for (int i = 0; i < 200; i++) {
        sceKernelUsleep(5000);
        const unsigned pub = video_bench_published();
        const uint64_t now = sceKernelGetProcessTime();
        if (pub != last_pub) {
            last_pub = pub;
            still_since = now;
        } else if (video_queued() == 0 && now - still_since > 100000) {
            break;
        }
    }
    VideoBenchResult r;
    video_bench_end(&r);
    const double elapsed = (t_pushed - t0) / 1e6;
    g_active = false;
    video_reset();
    video_bench_set_options(nullptr);
    int k = snprintf(out, n,
                     "{\"name\":\"%s\",\"ok\":true,\"frames\":%d,\"received\":%u,\"decoded\":%u,\"published\":%u,"
                     "\"dropped\":%u,\"replaced\":%u,\"errors\":%u,\"elapsed_s\":%.3f,\"fps_out\":%.2f,"
                     "\"kb_per_frame\":%.1f,\"depth\":%d,\"jobs\":%d,",
                     h.name, frames * h.loops, r.received, r.decoded, r.published, r.dropped, r.replaced, r.errors,
                     elapsed, elapsed > 0 ? r.published / elapsed : 0.0,
                     r.received ? r.bytes / 1024.0 / r.received : 0.0, h.depth, h.jobs);
    k += times_json(out + k, n - k, "decode", r.decode);
    out[k++] = ',';
    k += times_json(out + k, n - k, "convert", r.convert);
    out[k++] = ',';
    k += times_json(out + k, n - k, "latency", r.latency);
    snprintf(out + k, n - k, "}");
    LOG("bench: %s: %.1f fps out, decode avg %.2f p99 %.2f ms, convert avg %.2f ms, latency avg %.1f ms, dropped %u, "
        "replaced %u",
        h.name, elapsed > 0 ? r.published / elapsed : 0.0, r.decode.avg, r.decode.p99, r.convert.avg, r.latency.avg,
        r.dropped, r.replaced);
}

static void serve(int fd)
{
    static char header_json[2048], result[2048];
    static uint8_t config[1024];
    for (;;) {
        char magic[4];
        uint32_t len;
        if (!recv_all(fd, magic, 4) || memcmp(magic, "ALVB", 4) != 0 || !recv_all(fd, &len, 4) ||
            len >= sizeof(header_json) || !recv_all(fd, header_json, len))
            return;
        header_json[len] = 0;
        Header h;
        uint32_t config_len, frames;
        if (!parse_header(header_json, &h) || !recv_all(fd, &config_len, 4) || config_len > sizeof(config) ||
            !recv_all(fd, config, config_len) || !recv_all(fd, &frames, 4) || frames == 0 || frames > FRAMES_MAX) {
            send_json(fd, "{\"ok\":false,\"error\":\"bad header\"}");
            return;
        }
        size_t used = 0;
        for (uint32_t i = 0; i < frames; i++) {
            uint32_t w;
            if (!recv_all(fd, &w, 4))
                return;
            const uint32_t n = w & 0x7fffffff;
            if (used + n > CLIP_MAX) {
                send_json(fd, "{\"ok\":false,\"error\":\"clip larger than 160 MB\"}");
                return;
            }
            if (!recv_all(fd, g_clip + used, n))
                return;
            g_frame_off[i] = (uint32_t)used;
            g_frame_len[i] = n;
            g_frame_idr[i] = (w & 0x80000000u) != 0;
            used += n;
        }
        run_clip(h, config, config_len, (int)frames, result, sizeof(result));
        if (!send_json(fd, result))
            return;
    }
}

static void *bench_thread(void *)
{
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(BENCH_PORT);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(listener, (sockaddr *)&a, sizeof(a)) != 0 || listen(listener, 1) != 0) {
        LOG("bench: listen on TCP %d failed", BENCH_PORT);
        return nullptr;
    }
    LOG("bench: video bench listening on TCP %d", BENCH_PORT);
    for (;;) {
        int fd = accept(listener, nullptr, nullptr);
        if (fd < 0) {
            sceKernelUsleep(100000);
            continue;
        }
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
        LOG("bench: PC connected");
        serve(fd);
        g_active = false;
        close(fd);
        LOG("bench: PC disconnected");
    }
    return nullptr;
}

void bench_start()
{
#if ALVR_PS4_DEV
    off_t phys = 0;
    void *mem = nullptr;
    if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), CLIP_MAX, 0x10000, 0, &phys) < 0 ||
        sceKernelMapDirectMemory(&mem, CLIP_MAX, 0x33, 0, phys, 0x10000) < 0) {
        LOG("bench: no memory for the clip buffer");
        return;
    }
    g_clip = (uint8_t *)mem;
    pthread_t t;
    pthread_create(&t, nullptr, bench_thread, nullptr);
#endif
}
