#include "alvr_client.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <orbis/libkernel.h>

#include "bincode.h"
#include "log.h"
#include "audio.h"
#include "video.h"

// ---------------------------------------------------------------------------------------
// Constants (docs/alvr-20.14.1-protocol.md)

static const uint16_t CONTROL_PORT = 9943;
static const uint64_t PROTOCOL_ID = 0x17667eafcf6d5d67ull; // hash_string("20")
static const uint64_t KEEPALIVE_INTERVAL_US = 500000;
static const uint64_t KEEPALIVE_TIMEOUT_US = 2000000;
static const uint64_t HANDSHAKE_TIMEOUT_US = 2000000;
static const int SHARD_PREFIX = 18;

// OpenOrbis' MSG_* flags use values that mean something else to the PS4's FreeBSD
// kernel (its MSG_DONTWAIT is FreeBSD's MSG_WAITALL), and O_NONBLOCK set with fcntl
// had no effect (accept() blocked the announce loop after the first announce). Every
// accept/recv is therefore preceded by poll() with a timeout. SO_NOSIGPIPE is FreeBSD's.
#ifndef SO_NOSIGPIPE
#define SO_NOSIGPIPE 0x0800
#endif

enum StreamId : uint16_t { STREAM_TRACKING = 0, STREAM_HAPTICS = 1, STREAM_AUDIO = 2, STREAM_VIDEO = 3, STREAM_STATS = 4 };

// ClientControlPacket / ServerControlPacket variant indices.
enum : uint32_t { C_PLAYSPACE_SYNC = 0, C_REQUEST_IDR = 1, C_KEEPALIVE = 2, C_STREAM_READY = 3, C_VIEWS_CONFIG = 4,
                  C_BATTERY = 5, C_BUTTONS = 7, C_RESERVED = 10 };
enum : uint32_t { S_START_STREAM = 0, S_DECODER_CONFIG = 1, S_RESTARTING = 2, S_KEEPALIVE = 3 };

// ---------------------------------------------------------------------------------------
// State

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static AlvrStatus g_status;
static char g_hostname[33];
static char g_local_ip[16];
static AlvrViews g_views;
static AlvrHapticsCallback g_haptics;

static int g_control = -1;   // TCP control socket (net thread owns reads)
static int g_stream = -1;    // UDP stream socket
static int g_packet_size = 1400;
static uint32_t g_tx_index[5];
static uint64_t g_ids_head, g_ids_hand[2];

// Buttons: last sent values per path id, and a pending queue flushed by the net thread.
struct ButtonEntry {
    uint64_t id;
    bool scalar;
    float value;
};
static ButtonEntry g_pending[64];
static int g_pending_count;
static AlvrHandInput g_last_input[2];
static bool g_input_sent_once[2];

struct HandPaths {
    uint64_t menu, squeeze, trigger_click, trigger_value, stick_x, stick_y, stick_click, stick_touch, system;
};
static HandPaths g_paths[2];

static uint64_t now_us() { return sceKernelGetProcessTime(); }

// True if fd has data (or a pending connection) within timeout_ms.
static bool readable(int fd, int timeout_ms)
{
    pollfd p{fd, POLLIN, 0};
    return poll(&p, 1, timeout_ms) > 0 && (p.revents & (POLLIN | POLLHUP | POLLERR));
}

static int g_listener = -1;

static void set_state(AlvrState s)
{
    pthread_mutex_lock(&g_lock);
    g_status.state = s;
    pthread_mutex_unlock(&g_lock);
}

// ---------------------------------------------------------------------------------------
// Control socket framing: u32 BE length + bincode payload

static bool send_all(int fd, const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    while (n) {
        ssize_t r = send(fd, b, n, 0); // SIGPIPE is ignored (see alvr_start)
        if (r <= 0)
            return false;
        b += r;
        n -= (size_t)r;
    }
    return true;
}

static pthread_mutex_t g_control_send_lock = PTHREAD_MUTEX_INITIALIZER;

static bool control_send(const BinWriter &w)
{
    if (w.overflow || g_control < 0)
        return false;
    uint8_t prefix[4] = {(uint8_t)(w.len >> 24), (uint8_t)(w.len >> 16), (uint8_t)(w.len >> 8), (uint8_t)w.len};
    pthread_mutex_lock(&g_control_send_lock);
    bool ok = send_all(g_control, prefix, 4) && send_all(g_control, w.buf, w.len);
    pthread_mutex_unlock(&g_control_send_lock);
    return ok;
}

static bool control_send_unit(uint32_t variant)
{
    uint8_t buf[8];
    BinWriter w{buf, sizeof(buf), 0, false};
    w.variant(variant);
    return control_send(w);
}

// Receives one framed message into *buf (grown as needed). Returns payload length,
// 0 on timeout, -1 on error/close.
static long control_recv(uint8_t **buf, size_t *cap, uint64_t timeout_us)
{
    uint64_t deadline = now_us() + timeout_us;
    uint8_t prefix[4];
    size_t got = 0;
    size_t need = 4;
    uint32_t len = 0;
    bool have_len = false;
    while (true) {
        uint8_t *dst = have_len ? *buf + got : prefix + got;
        uint64_t t = now_us();
        if (t >= deadline)
            return got == 0 && !have_len ? 0 : -1;
        int wait_ms = (int)((deadline - t + 999) / 1000);
        if (!readable(g_control, wait_ms))
            continue; // re-check the deadline
        ssize_t r = recv(g_control, dst, need - got, 0);
        if (r <= 0)
            return -1; // readable but nothing: closed or reset
        got += (size_t)r;
        if (got < need)
            continue;
        if (!have_len) {
            len = (uint32_t)prefix[0] << 24 | (uint32_t)prefix[1] << 16 | (uint32_t)prefix[2] << 8 | prefix[3];
            if (len > 16 * 1024 * 1024)
                return -1;
            if (len > *cap) {
                *buf = (uint8_t *)realloc(*buf, len);
                *cap = len;
            }
            have_len = true;
            got = 0;
            need = len;
            if (len == 0)
                return 0;
            deadline = now_us() + timeout_us; // the rest of the frame follows quickly
            continue;
        }
        return (long)len;
    }
}

// ---------------------------------------------------------------------------------------
// Stream socket: 18-byte BE shard prefix, chunks of D = packet_size + 4 - 18 bytes

static void put_be16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = (uint8_t)v; }
static void put_be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = (uint8_t)v; }
static uint32_t get_be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static pthread_mutex_t g_stream_send_lock = PTHREAD_MUTEX_INITIALIZER;

static void stream_send(uint16_t stream, const uint8_t *data, size_t len)
{
    if (g_stream < 0 || len == 0)
        return;
    const size_t d = (size_t)g_packet_size + 4 - SHARD_PREFIX;
    const uint32_t shards = (uint32_t)((len + d - 1) / d);
    uint8_t pkt[2048];
    pthread_mutex_lock(&g_stream_send_lock);
    uint32_t index = g_tx_index[stream]++;
    for (uint32_t i = 0; i < shards; i++) {
        size_t off = i * d, chunk = len - off < d ? len - off : d;
        put_be32(pkt, (uint32_t)(14 + chunk));
        put_be16(pkt + 4, stream);
        put_be32(pkt + 6, index);
        put_be32(pkt + 10, shards);
        put_be32(pkt + 14, i);
        memcpy(pkt + SHARD_PREFIX, data + off, chunk);
        send(g_stream, pkt, SHARD_PREFIX + chunk, 0);
    }
    pthread_mutex_unlock(&g_stream_send_lock);
}

// Packet reassembly (docs section 1.4), one per stream: one packet in flight; a shard of
// a newer packet abandons an incomplete one. Any gap in delivered packet indices is a loss.
struct Reassembler {
    size_t max_packet;
    uint32_t max_shards;
    uint8_t *buf, *have;
    bool active, have_last;
    uint32_t index, shards, got, last;
    size_t len;
};

// Returns true when the packet `index` is complete (data in r.buf, r.len bytes).
static bool reassemble(Reassembler &r, uint32_t index, uint32_t shards, uint32_t shard, const uint8_t *chunk,
                       size_t chunk_len, bool *loss)
{
    const size_t d = (size_t)g_packet_size + 4 - SHARD_PREFIX;
    if (!r.buf) {
        r.buf = (uint8_t *)malloc(r.max_packet);
        r.have = (uint8_t *)malloc(r.max_shards);
    }
    if (!r.buf || !r.have || shards == 0 || shards > r.max_shards || shard >= shards || chunk_len > d ||
        (size_t)shards * d > r.max_packet)
        return false;
    if (r.have_last && (int32_t)(index - r.last) <= 0)
        return false; // already delivered or older
    if (!r.active || index != r.index) {
        if (r.active && (int32_t)(index - r.index) < 0)
            return false; // late shard of an abandoned packet
        r.active = true;
        r.index = index;
        r.shards = shards;
        r.got = 0;
        r.len = 0;
        memset(r.have, 0, shards);
    }
    if (shards != r.shards || r.have[shard])
        return false;
    r.have[shard] = 1;
    r.got++;
    memcpy(r.buf + shard * d, chunk, chunk_len);
    if (shard == shards - 1)
        r.len = shard * d + chunk_len;
    if (r.got < r.shards)
        return false;
    r.active = false;
    *loss = r.have_last && index != r.last + 1;
    r.have_last = true;
    r.last = index;
    return true;
}

static Reassembler g_video_rx{3 << 20, 4096};
static Reassembler g_audio_rx{256 << 10, 256};

static void reassembly_reset()
{
    g_video_rx.active = g_video_rx.have_last = false;
    g_audio_rx.active = g_audio_rx.have_last = false;
}

static void video_shard(uint32_t index, uint32_t shards, uint32_t shard, const uint8_t *chunk, size_t chunk_len)
{
    bool loss = false;
    if (!reassemble(g_video_rx, index, shards, shard, chunk, chunk_len, &loss))
        return;
    if (loss) // P-frames would reference missing data: drop until the next IDR
        video_packet_loss();
    // VideoPacketHeader { timestamp: Duration (u64 secs, u32 nanos), is_idr: bool }
    const uint8_t *b = g_video_rx.buf;
    if (g_video_rx.len < 13)
        return;
    uint64_t secs;
    uint32_t nanos;
    memcpy(&secs, b, 8);
    memcpy(&nanos, b + 8, 4);
    bool idr = b[12] != 0;
    video_push_frame(secs * 1000000000ull + nanos, idr, b + 13, g_video_rx.len - 13);
    pthread_mutex_lock(&g_lock);
    g_status.video_packets++;
    pthread_mutex_unlock(&g_lock);
}

// Game audio: header () then s16le stereo PCM.
static void audio_shard(uint32_t index, uint32_t shards, uint32_t shard, const uint8_t *chunk, size_t chunk_len)
{
    bool loss = false;
    if (reassemble(g_audio_rx, index, shards, shard, chunk, chunk_len, &loss))
        audio_push_game(g_audio_rx.buf, g_audio_rx.len);
}

static void stream_poll()
{
    uint8_t pkt[4096];
    for (int n = 0; n < 512 && readable(g_stream, 0); n++) {
        ssize_t r = recv(g_stream, pkt, sizeof(pkt), 0);
        if (r < SHARD_PREFIX)
            return;
        uint16_t stream = (uint16_t)(pkt[4] << 8 | pkt[5]);
        uint32_t shards = get_be32(pkt + 10), shard = get_be32(pkt + 14);
        const uint8_t *chunk = pkt + SHARD_PREFIX;
        size_t chunk_len = (size_t)r - SHARD_PREFIX;
        if (stream == STREAM_HAPTICS && shards == 1) {
            // Haptics { device_id u64, duration, frequency f32, amplitude f32 }
            BinReader rd{chunk, chunk_len, 0, false};
            uint64_t dev = rd.u64();
            uint64_t dur_ns = rd.duration_ns();
            float freq = rd.f32(), amp = rd.f32();
            if (!rd.error && g_haptics) {
                int hand = dev == g_ids_hand[0] ? 0 : dev == g_ids_hand[1] ? 1 : -1;
                if (hand >= 0)
                    g_haptics(hand, dur_ns / 1e9f, freq, amp);
            }
        } else if (stream == STREAM_VIDEO) {
            video_shard(get_be32(pkt + 6), shards, shard, chunk, chunk_len);
        } else if (stream == STREAM_AUDIO) {
            audio_shard(get_be32(pkt + 6), shards, shard, chunk, chunk_len);
        }
    }
}

// ---------------------------------------------------------------------------------------
// Handshake helpers

// Finds `"key":` in a compact serde_json document and returns a pointer to the value.
static const char *json_find(const char *json, size_t len, const char *key)
{
    char pat[96];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    size_t pl = strlen(pat);
    for (size_t i = 0; i + pl <= len; i++)
        if (memcmp(json + i, pat, pl) == 0)
            return json + i + pl;
    return nullptr;
}

static void write_capabilities(BinWriter &w)
{
    static const char *caps =
        "{\"default_view_resolution\":[960,1080],\"supported_refresh_rates\":[60.0],"
        "\"microphone_sample_rate\":48000,\"supports_foveated_encoding\":false,\"encoder_high_profile\":true,"
        "\"encoder_10_bits\":false,\"encoder_av1\":false,\"multimodal_protocol\":false,\"prefer_10bit\":false,"
        "\"prefer_full_range\":true,\"preferred_encoding_gamma\":1.0,\"prefer_hdr\":false}";
    w.u8(1); // Some(VideoStreamingCapabilitiesLegacy)
    w.u32(960);
    w.u32(1080);
    size_t n = strlen(caps);
    // 60 Hz only: the hardware decoder takes ~13.5 ms per 1920x1056 frame, too slow for
    // 90 Hz. The system compositor reprojects 60 Hz to the PSVR's 120 Hz, as games do.
    w.u64(1 + n);
    w.f32(60.0f);
    for (size_t i = 0; i < n; i++)
        w.f32(-(float)(uint8_t)caps[i]); // JSON smuggled as negative "refresh rates"
    w.u32(48000);
}

static bool send_connection_accepted(uint32_t server_ip_be)
{
    static uint8_t buf[4096];
    BinWriter w{buf, sizeof(buf), 0, false};
    w.variant(0); // ConnectionAccepted
    w.u64(PROTOCOL_ID);
    w.str("PlayStation VR");
    w.u32(0); // IpAddr::V4
    w.bytes(&server_ip_be, 4);
    write_capabilities(w);
    return control_send(w);
}

static void send_views_config()
{
    uint8_t buf[64];
    BinWriter w{buf, sizeof(buf), 0, false};
    w.variant(C_VIEWS_CONFIG);
    w.f32(g_views.ipd_m);
    for (int e = 0; e < 2; e++)
        for (int k = 0; k < 4; k++)
            w.f32(g_views.fov[e][k]);
    control_send(w);
}

static void send_custom_interaction_profile()
{
    // Option A (docs 7.3): declare Touch-like source inputs so the streamer's automatic
    // Vive-wand bindings forward thumbstick -> trackpad. Ids exceed 2^53: printed as u64.
    char json[1024];
    int n = snprintf(json, sizeof(json), "{\"CustomInteractionProfile\":{\"device_id\":%llu,\"input_ids\":[",
                     (unsigned long long)g_ids_hand[0]);
    for (int h = 0; h < 2; h++) {
        const HandPaths &p = g_paths[h];
        const uint64_t ids[] = {p.menu, p.squeeze, p.trigger_click, p.trigger_value, p.stick_x, p.stick_y,
                                p.stick_click, p.stick_touch, p.system};
        for (unsigned i = 0; i < sizeof(ids) / sizeof(ids[0]); i++)
            n += snprintf(json + n, sizeof(json) - n, "%s%llu", h == 0 && i == 0 ? "" : ",",
                          (unsigned long long)ids[i]);
    }
    snprintf(json + n, sizeof(json) - n, "]}}");
    uint8_t buf[1100];
    BinWriter w{buf, sizeof(buf), 0, false};
    w.variant(C_RESERVED);
    w.str(json);
    control_send(w);
}

static void send_playspace_sync()
{
    uint8_t buf[16];
    BinWriter w{buf, sizeof(buf), 0, false};
    w.variant(C_PLAYSPACE_SYNC);
    w.u8(1); // Some(Vec2)
    w.f32(2.0f);
    w.f32(2.0f);
    control_send(w);
}

static void flush_buttons()
{
    pthread_mutex_lock(&g_lock);
    int n = g_pending_count;
    ButtonEntry entries[64];
    memcpy(entries, g_pending, sizeof(ButtonEntry) * n);
    g_pending_count = 0;
    pthread_mutex_unlock(&g_lock);
    if (!n)
        return;
    uint8_t buf[1024];
    BinWriter w{buf, sizeof(buf), 0, false};
    w.variant(C_BUTTONS);
    w.u64((uint64_t)n);
    for (int i = 0; i < n; i++) {
        w.u64(entries[i].id);
        if (entries[i].scalar) {
            w.variant(1);
            w.f32(entries[i].value);
        } else {
            w.variant(0);
            w.boolean(entries[i].value != 0.0f);
        }
    }
    control_send(w);
}

// ---------------------------------------------------------------------------------------
// Session: one connection from accept to disconnect

static void close_fd(int *fd)
{
    if (*fd >= 0)
        close(*fd);
    *fd = -1;
}

static void run_session(int fd, uint32_t server_ip_be)
{
    g_control = fd;
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    char ip[16];
    inet_ntop(AF_INET, &server_ip_be, ip, sizeof(ip));
    pthread_mutex_lock(&g_lock);
    snprintf(g_status.server_ip, sizeof(g_status.server_ip), "%s", ip);
    g_status.video_packets = 0;
    pthread_mutex_unlock(&g_lock);
    set_state(ALVR_HANDSHAKE);
    LOG("alvr: streamer %s connected, sending ConnectionAccepted", ip);

    uint8_t *buf = nullptr;
    size_t cap = 0;
    long len;
    if (!send_connection_accepted(server_ip_be)) {
        LOG("alvr: send ConnectionAccepted failed");
        goto end;
    }
    // StreamConfigPacket { session: String, negotiated: String }
    len = control_recv(&buf, &cap, HANDSHAKE_TIMEOUT_US);
    if (len <= 0) {
        LOG("alvr: no StreamConfigPacket (%ld)", len);
        goto end;
    }
    {
        BinReader rd{buf, (size_t)len, 0, false};
        uint64_t slen, nlen;
        const char *session = (const char *)rd.blob(&slen);
        const char *neg = (const char *)rd.blob(&nlen);
        if (rd.error) {
            LOG("alvr: bad StreamConfigPacket");
            goto end;
        }
        LOG("alvr: StreamConfigPacket session %llu bytes, negotiated: %.*s", (unsigned long long)slen, (int)nlen, neg);
        if (const char *v = json_find(session, slen, "packet_size"))
            g_packet_size = atoi(v);
        int stream_port = 9944;
        if (const char *v = json_find(session, slen, "stream_port"))
            stream_port = atoi(v);
        const char *proto = json_find(session, slen, "stream_protocol");
        bool udp = !proto || strncmp(proto, "{\"variant\":\"Udp\"", 16) == 0;
        uint32_t vw = 0, vh = 0;
        float fps = 0;
        if (const char *v = json_find(neg, nlen, "view_resolution"))
            sscanf(v, "[%u,%u]", &vw, &vh);
        if (const char *v = json_find(neg, nlen, "refresh_rate_hint"))
            fps = strtof(v, nullptr);
        bool full_range = true;
        if (const char *v = json_find(neg, nlen, "use_full_range"))
            full_range = strncmp(v, "true", 4) == 0;
        video_reset();
        reassembly_reset();
        video_set_stream(vw, vh, full_range);
        uint32_t game_audio_rate = 0;
        if (const char *v = json_find(neg, nlen, "game_audio_sample_rate"))
            game_audio_rate = (uint32_t)atoi(v);
        // Session settings: "microphone":{"enabled":...}
        bool microphone = false;
        if (const char *v = json_find(session, slen, "microphone"))
            microphone = strncmp(v, "{\"enabled\":true", 15) == 0;
        LOG("alvr: packet_size=%d stream_port=%d protocol=%s view=%ux%u fps=%.1f", g_packet_size, stream_port,
            udp ? "UDP" : "TCP (unsupported)", vw, vh, fps);
        pthread_mutex_lock(&g_lock);
        g_status.view_width = vw;
        g_status.view_height = vh;
        g_status.refresh_rate = fps;
        pthread_mutex_unlock(&g_lock);

        // StartStream or Restarting
        len = control_recv(&buf, &cap, HANDSHAKE_TIMEOUT_US);
        if (len < 4) {
            LOG("alvr: no StartStream (%ld)", len);
            goto end;
        }
        uint32_t variant = *(uint32_t *)buf;
        if (variant == S_RESTARTING) {
            LOG("alvr: streamer is restarting SteamVR, reconnecting after it");
            set_state(ALVR_RESTARTING);
            goto end;
        }
        if (variant != S_START_STREAM) {
            LOG("alvr: unexpected packet %u instead of StartStream", variant);
            goto end;
        }
        if (!udp) {
            LOG("alvr: TCP streaming is not supported yet, set the streamer to UDP");
            goto end;
        }
        // Stream socket: bind local stream_port, connect to the streamer's.
        g_stream = socket(AF_INET, SOCK_DGRAM, 0);
        int big = 4 * 1024 * 1024;
        setsockopt(g_stream, SOL_SOCKET, SO_RCVBUF, &big, sizeof(big));
        sockaddr_in local{}, remote{};
        local.sin_family = AF_INET;
        local.sin_port = htons((uint16_t)stream_port);
        local.sin_addr.s_addr = htonl(INADDR_ANY);
        remote.sin_family = AF_INET;
        remote.sin_port = htons((uint16_t)stream_port);
        remote.sin_addr.s_addr = server_ip_be;
        if (bind(g_stream, (sockaddr *)&local, sizeof(local)) != 0 ||
            connect(g_stream, (sockaddr *)&remote, sizeof(remote)) != 0) {
            LOG("alvr: stream socket bind/connect failed");
            goto end;
        }
        fcntl(g_stream, F_SETFL, fcntl(g_stream, F_GETFL, 0) | O_NONBLOCK);
        memset(g_tx_index, 0, sizeof(g_tx_index));
        if (!control_send_unit(C_STREAM_READY))
            goto end;
        LOG("alvr: StreamReady sent, streaming");
        set_state(ALVR_STREAMING);
        audio_start_stream(game_audio_rate, microphone);
        send_views_config();
        send_custom_interaction_profile();
        send_playspace_sync();
        memset(g_input_sent_once, 0, sizeof(g_input_sent_once));
    }

    // Streaming loop: keepalive, control packets, stream socket.
    {
        uint64_t last_rx = now_us(), last_tx = 0;
        while (true) {
            uint64_t t = now_us();
            if (t - last_tx >= KEEPALIVE_INTERVAL_US) {
                if (!control_send_unit(C_KEEPALIVE)) {
                    LOG("alvr: control send failed");
                    break;
                }
                last_tx = t;
            }
            flush_buttons();
            if (video_want_idr()) {
                control_send_unit(C_REQUEST_IDR);
                LOG("alvr: IDR requested");
            }
            // Wait for either socket (4 ms max, which also paces this loop).
            pollfd fds[2] = {{g_control, POLLIN, 0}, {g_stream, POLLIN, 0}};
            poll(fds, 2, 4);
            len = 0;
            if (fds[0].revents & (POLLIN | POLLHUP | POLLERR))
                len = control_recv(&buf, &cap, 100000); // a started message completes quickly
            if (len < 0) {
                LOG("alvr: control socket closed");
                break;
            }
            if (len >= 4) {
                last_rx = now_us();
                uint32_t variant = *(uint32_t *)buf;
                if (variant == S_RESTARTING) {
                    LOG("alvr: streamer restarting");
                    set_state(ALVR_RESTARTING);
                    break;
                } else if (variant == S_DECODER_CONFIG) {
                    // DecoderInitializationConfig { codec: CodecType (u32), config_buffer: Vec<u8> }
                    BinReader rd{buf + 4, (size_t)len - 4, 0, false};
                    uint32_t codec = rd.u32();
                    uint64_t clen;
                    const uint8_t *cfg = rd.blob(&clen);
                    if (!rd.error)
                        video_set_decoder_config(codec, cfg, (size_t)clen);
                    else
                        LOG("alvr: bad DecoderConfig (%ld bytes)", len);
                }
            }
            if (now_us() - last_rx > KEEPALIVE_TIMEOUT_US) {
                LOG("alvr: keepalive timeout");
                break;
            }
            if (g_listener >= 0 && readable(g_listener, 0)) {
                LOG("alvr: the streamer opened a new connection, dropping this session");
                break;
            }
            stream_poll();
        }
    }
end:
    free(buf);
    pthread_mutex_lock(&g_control_send_lock);
    close_fd(&g_control);
    pthread_mutex_unlock(&g_control_send_lock);
    pthread_mutex_lock(&g_stream_send_lock);
    close_fd(&g_stream);
    pthread_mutex_unlock(&g_stream_send_lock);
    video_reset();
    audio_stop_stream();
    LOG("alvr: disconnected");
    if (g_status.state != ALVR_RESTARTING)
        set_state(ALVR_DISCOVERY);
}

// ---------------------------------------------------------------------------------------
// Discovery thread: announce on UDP 9943 every second, accept the streamer on TCP 9943

static void *net_thread(void *)
{
    int announce = socket(AF_INET, SOCK_DGRAM, 0);
    int one = 1;
    setsockopt(announce, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
    setsockopt(announce, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(CONTROL_PORT);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(announce, (sockaddr *)&a, sizeof(a)) != 0)
        LOG("alvr: announce socket bind failed");

    uint8_t packet[56];
    memset(packet, 0, sizeof(packet));
    memcpy(packet, "ALVR", 4);
    memcpy(packet + 16, &PROTOCOL_ID, 8);
    memcpy(packet + 24, g_hostname, strlen(g_hostname));
    sockaddr_in bcast{};
    bcast.sin_family = AF_INET;
    bcast.sin_port = htons(CONTROL_PORT);
    bcast.sin_addr.s_addr = htonl(INADDR_BROADCAST);

    int listener = g_listener = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (bind(listener, (sockaddr *)&a, sizeof(a)) != 0 || listen(listener, 1) != 0)
        LOG("alvr: control listener bind/listen failed");
    fcntl(listener, F_SETFL, fcntl(listener, F_GETFL, 0) | O_NONBLOCK);
    LOG("alvr: announcing %s (protocol 20) on UDP %u, listening on TCP %u", g_hostname, CONTROL_PORT, CONTROL_PORT);

    // Also send to the subnet broadcast (x.y.z.255 for a /24): the limited broadcast
    // 255.255.255.255 did not reliably reach the streamer.
    sockaddr_in subnet = bcast;
    if (g_local_ip[0]) {
        in_addr ip{};
        if (inet_pton(AF_INET, g_local_ip, &ip) == 1) {
            subnet.sin_addr.s_addr = (ip.s_addr & htonl(0xffffff00)) | htonl(0xff);
        }
    }
    // The streamer registers the controllers at the end of the handshake and gives SteamVR
    // only 1 s to activate each one; if that times out, the controller never gets a pose
    // and stays greyed in SteamVR. Right after SteamVR starts, the streamer connects while
    // SteamVR is still loading the other drivers (activation then took ~2 s), so the first
    // connection after a period without session is refused for HOLDOFF_US; the streamer
    // retries every second on its own.
    const uint64_t HOLDOFF_US = 6000000;
    uint64_t holdoff_until = 0; // 0: armed, the next connection starts the holdoff
    uint64_t last_announce = 0;
    unsigned announces = 0;
    for (;;) {
        uint64_t t = now_us();
        if (t - last_announce >= 1000000) {
            ssize_t r1 = sendto(announce, packet, sizeof(packet), 0, (sockaddr *)&bcast, sizeof(bcast));
            ssize_t r2 = sendto(announce, packet, sizeof(packet), 0, (sockaddr *)&subnet, sizeof(subnet));
            if (announces++ % 30 == 0)
                LOG("alvr: announce #%u -> 255.255.255.255: %d, subnet: %d", announces, (int)r1, (int)r2);
            last_announce = t;
        }
        if (holdoff_until != 0 && t > holdoff_until + 5000000) {
            holdoff_until = 0; // the streamer stopped retrying (SteamVR closed): re-arm
            set_state(ALVR_DISCOVERY);
        }
        if (!readable(listener, 100))
            continue;
        sockaddr_in peer{};
        socklen_t plen = sizeof(peer);
        int fd = accept(listener, (sockaddr *)&peer, &plen);
        if (fd >= 0) {
            uint64_t now = now_us();
            if (holdoff_until == 0 || now < holdoff_until) {
                if (holdoff_until == 0) {
                    holdoff_until = now + HOLDOFF_US;
                    char ip[16];
                    inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
                    pthread_mutex_lock(&g_lock);
                    snprintf(g_status.server_ip, sizeof(g_status.server_ip), "%s", ip);
                    pthread_mutex_unlock(&g_lock);
                    set_state(ALVR_WAITING);
                    LOG("alvr: streamer %s found, letting SteamVR load for %u s", ip,
                        (unsigned)(HOLDOFF_US / 1000000));
                }
                close(fd);
                continue;
            }
            run_session(fd, peer.sin_addr.s_addr);
            holdoff_until = 0; // SteamVR may restart (or be restarted) before the next session
            if (g_status.state == ALVR_RESTARTING) {
                sceKernelUsleep(1000000);
                set_state(ALVR_DISCOVERY);
            }
            continue;
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------------------
// Public API

static void init_paths()
{
    g_ids_head = alvr_hash_string("/user/head");
    const char *hands[2] = {"left", "right"};
    for (int h = 0; h < 2; h++) {
        char p[96];
        snprintf(p, sizeof(p), "/user/hand/%s", hands[h]);
        g_ids_hand[h] = alvr_hash_string(p);
        auto id = [&](const char *suffix) {
            snprintf(p, sizeof(p), "/user/hand/%s/input/%s", hands[h], suffix);
            return alvr_hash_string(p);
        };
        HandPaths &hp = g_paths[h];
        hp.menu = id("menu/click");
        hp.squeeze = id("squeeze/click");
        hp.trigger_click = id("trigger/click");
        hp.trigger_value = id("trigger/value");
        hp.stick_x = id("thumbstick/x");
        hp.stick_y = id("thumbstick/y");
        hp.stick_click = id("thumbstick/click");
        hp.stick_touch = id("thumbstick/touch");
        hp.system = id("system/click");
    }
}

void alvr_start(const char *hostname, const char *local_ip, const AlvrViews *views, AlvrHapticsCallback haptics)
{
    snprintf(g_hostname, sizeof(g_hostname), "%s", hostname);
    snprintf(g_local_ip, sizeof(g_local_ip), "%s", local_ip);
    g_views = *views;
    g_haptics = haptics;
    memset(&g_status, 0, sizeof(g_status));
    init_paths();
    // A send() on a socket the streamer closed raises SIGPIPE, which kills the app
    // (CE-34878-0 right after a SteamVR restart). Ignore it; errors come back as EPIPE.
    signal(SIGPIPE, SIG_IGN);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 512 * 1024);
    pthread_t th;
    pthread_create(&th, &attr, net_thread, nullptr);
    pthread_attr_destroy(&attr);
}

void alvr_get_status(AlvrStatus *out)
{
    pthread_mutex_lock(&g_lock);
    *out = g_status;
    pthread_mutex_unlock(&g_lock);
}

static void write_motion(BinWriter &w, uint64_t id, const AlvrDeviceMotion *m)
{
    w.u64(id);
    for (int i = 0; i < 4; i++)
        w.f32(m->orientation[i]);
    for (int i = 0; i < 3; i++)
        w.f32(m->position[i]);
    for (int i = 0; i < 3; i++)
        w.f32(m->linear_velocity[i]);
    for (int i = 0; i < 3; i++)
        w.f32(m->angular_velocity[i]);
}

void alvr_send_microphone(const uint8_t *pcm, size_t len)
{
    if (g_status.state == ALVR_STREAMING)
        stream_send(STREAM_AUDIO, pcm, len); // header () + s16le mono PCM
}

void alvr_send_tracking(uint64_t timestamp_ns, const AlvrDeviceMotion *head, const AlvrDeviceMotion *left,
                        const AlvrDeviceMotion *right)
{
    if (g_status.state != ALVR_STREAMING)
        return;
    uint8_t buf[512];
    BinWriter w{buf, sizeof(buf), 0, false};
    w.duration_ns(timestamp_ns);
    uint64_t count = 1 + (left && left->present) + (right && right->present);
    w.u64(count);
    write_motion(w, g_ids_head, head); // the head must always be sent
    if (left && left->present)
        write_motion(w, g_ids_hand[0], left);
    if (right && right->present)
        write_motion(w, g_ids_hand[1], right);
    for (int i = 0; i < 7; i++)
        w.u8(0); // hand skeletons x2, eye gazes x2, face expressions x3: None
    stream_send(STREAM_TRACKING, buf, w.len);
    static uint64_t last_log = 0;
    if (timestamp_ns - last_log > 5000000000ull) {
        last_log = timestamp_ns;
        LOG("alvr: tracking #%u: head p=(%.2f %.2f %.2f), left %s p=(%.2f %.2f %.2f), right %s p=(%.2f %.2f %.2f)",
            g_tx_index[STREAM_TRACKING], head->position[0], head->position[1], head->position[2],
            left && left->present ? "sent" : "omitted", left ? left->position[0] : 0, left ? left->position[1] : 0,
            left ? left->position[2] : 0, right && right->present ? "sent" : "omitted",
            right ? right->position[0] : 0, right ? right->position[1] : 0, right ? right->position[2] : 0);
    }
}

static void queue_button(uint64_t id, bool scalar, float value)
{
    if (g_pending_count < 64)
        g_pending[g_pending_count++] = ButtonEntry{id, scalar, value};
}

void alvr_update_input(int hand, const AlvrHandInput *in)
{
    if (g_status.state != ALVR_STREAMING || hand < 0 || hand > 1)
        return;
    pthread_mutex_lock(&g_lock);
    const AlvrHandInput &o = g_last_input[hand];
    const HandPaths &p = g_paths[hand];
    const bool all = !g_input_sent_once[hand];
    // Trackpad travels as thumbstick (streamer maps it to the Vive trackpad), see docs 7.3.
    if (all || in->menu != o.menu)
        queue_button(p.menu, false, in->menu);
    if (all || in->grip != o.grip)
        queue_button(p.squeeze, false, in->grip);
    if (all || in->trigger_click != o.trigger_click)
        queue_button(p.trigger_click, false, in->trigger_click);
    if (all || in->trigger != o.trigger)
        queue_button(p.trigger_value, true, in->trigger);
    if (all || in->trackpad_x != o.trackpad_x)
        queue_button(p.stick_x, true, in->trackpad_x);
    if (all || in->trackpad_y != o.trackpad_y)
        queue_button(p.stick_y, true, in->trackpad_y);
    if (all || in->trackpad_click != o.trackpad_click)
        queue_button(p.stick_click, false, in->trackpad_click);
    if (all || in->trackpad_touch != o.trackpad_touch)
        queue_button(p.stick_touch, false, in->trackpad_touch);
    if (all || in->system != o.system)
        queue_button(p.system, false, in->system);
    g_last_input[hand] = *in;
    g_input_sent_once[hand] = true;
    pthread_mutex_unlock(&g_lock);
}
