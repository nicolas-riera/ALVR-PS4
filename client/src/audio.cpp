#include "audio.h"

#include <pthread.h>
#include <string.h>

#include <orbis/libkernel.h>

#include "log.h"

typedef int (*AudioOutInitFn)();
typedef int (*AudioOutOpenFn)(int32_t user, int32_t port, int32_t index, uint32_t len, uint32_t freq, uint32_t param);
typedef int (*AudioOutOutputFn)(int32_t handle, const void *ptr);
typedef int (*AudioInOpenFn)(int32_t user, uint32_t type, uint32_t index, uint32_t len, uint32_t freq, uint32_t param);
typedef int (*AudioInInputFn)(int32_t handle, void *dest);
typedef int (*AudioInCloseFn)(int32_t handle);

static AudioOutInitFn p_out_init;
static AudioOutOpenFn p_out_open;
static AudioOutOutputFn p_out_output;
static AudioInOpenFn p_in_open;
static AudioInInputFn p_in_input;
static AudioInCloseFn p_in_close;

static const uint32_t RATE = 48000;          // the only rate of the output port
static const uint32_t GRAIN = 256;           // frames per sceAudioOutOutput / sceAudioInInput
static const uint32_t RING_FRAMES = 16384;   // 341 ms
// Jitter buffer, in frames: playback starts once START are queued; above MAX (clock drift
// or a burst) the oldest frames are dropped back down to START.
static const uint32_t START_FRAMES = RATE * 40 / 1000;
static const uint32_t MAX_FRAMES = RATE * 150 / 1000;

static const uint32_t USER_SYSTEM = 0xff;
static const uint32_t PORT_MAIN = 0, PORT_BGM = 1;
static const uint32_t FORMAT_S16_STEREO = 1, FORMAT_S16_MONO = 0;
static const uint32_t AUDIO_IN_VOICE_CHAT = 0, AUDIO_IN_GENERAL = 1;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int16_t g_ring[RING_FRAMES * 2];
static uint32_t g_ring_read, g_ring_count; // frames
static bool g_game_on, g_playing;
static unsigned g_underruns, g_overflows;

static int g_user_id;
static AudioMicSink g_mic_sink;
static volatile bool g_mic_on;

static void *resolve(int module, const char *name)
{
    void *fn = nullptr;
    if (module < 0 || sceKernelDlsym(module, name, &fn) != 0 || !fn) {
        LOG("audio: symbol %s not found", name);
        return nullptr;
    }
    return fn;
}

// ---------------------------------------------------------------------------------------
// Game audio playback

static void *playback_thread(void *)
{
    int h = p_out_open(USER_SYSTEM, PORT_MAIN, 0, GRAIN, RATE, FORMAT_S16_STEREO);
    if (h < 0) {
        LOG("audio: main port open -> 0x%08x, trying the BGM port", (unsigned)h);
        h = p_out_open(USER_SYSTEM, PORT_BGM, 0, GRAIN, RATE, FORMAT_S16_STEREO);
    }
    LOG("audio: output port -> 0x%08x", (unsigned)h);
    if (h < 0)
        return nullptr;
    static int16_t buf[GRAIN * 2];
    uint64_t last_log = 0;
    for (;;) {
        pthread_mutex_lock(&g_lock);
        if (!g_playing && g_game_on && g_ring_count >= START_FRAMES)
            g_playing = true;
        if (g_playing && g_ring_count < GRAIN) {
            g_playing = false; // underrun: silence until the buffer is refilled
            g_underruns++;
        }
        if (g_playing) {
            for (uint32_t i = 0; i < GRAIN; i++) {
                uint32_t r = (g_ring_read + i) % RING_FRAMES;
                buf[i * 2] = g_ring[r * 2];
                buf[i * 2 + 1] = g_ring[r * 2 + 1];
            }
            g_ring_read = (g_ring_read + GRAIN) % RING_FRAMES;
            g_ring_count -= GRAIN;
        } else {
            memset(buf, 0, sizeof(buf));
        }
        unsigned under = g_underruns, over = g_overflows, level = g_ring_count;
        bool on = g_game_on;
        pthread_mutex_unlock(&g_lock);
        p_out_output(h, buf); // blocks until the port has room: paces this loop
        uint64_t now = sceKernelGetProcessTime();
        if (on && now - last_log > 10000000) {
            LOG("audio: buffer %.1f ms, underruns %u, overflows %u", level * 1000.0 / RATE, under, over);
            last_log = now;
        }
    }
    return nullptr;
}

void audio_push_game(const uint8_t *pcm, size_t len)
{
    uint32_t frames = (uint32_t)(len / 4);
    pthread_mutex_lock(&g_lock);
    if (!g_game_on || frames == 0 || frames > RING_FRAMES) {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    if (g_ring_count + frames > MAX_FRAMES) {
        uint32_t keep = START_FRAMES > frames ? START_FRAMES - frames : 0;
        uint32_t drop = g_ring_count > keep ? g_ring_count - keep : 0;
        g_ring_read = (g_ring_read + drop) % RING_FRAMES;
        g_ring_count -= drop;
        g_overflows++;
    }
    uint32_t w = (g_ring_read + g_ring_count) % RING_FRAMES;
    for (uint32_t i = 0; i < frames; i++) {
        uint32_t d = (w + i) % RING_FRAMES;
        memcpy(&g_ring[d * 2], pcm + i * 4, 4); // s16le L, R
    }
    g_ring_count += frames;
    pthread_mutex_unlock(&g_lock);
}

// ---------------------------------------------------------------------------------------
// Microphone

static void *mic_thread(void *)
{
    int h = -1;
    uint32_t in_rate = RATE;
    static int16_t in[GRAIN * 3];
    static int16_t out[2048];
    uint32_t out_len = 0;
    int failures = 0; // consecutive input errors
    const uint32_t CHUNK = RATE / 100; // 10 ms packets (960 bytes), within one shard
    for (;;) {
        if (!g_mic_on) {
            out_len = 0;
            sceKernelUsleep(20000);
            continue;
        }
        if (h < 0) {
            // The PSVR microphone is the system's input device while the headset is on.
            in_rate = RATE;
            h = p_in_open(g_user_id, AUDIO_IN_GENERAL, 0, GRAIN, RATE, FORMAT_S16_MONO);
            if (h < 0) {
                LOG("audio: microphone (general, 48 kHz) -> 0x%08x, trying voice chat 16 kHz", (unsigned)h);
                h = p_in_open(g_user_id, AUDIO_IN_VOICE_CHAT, 0, GRAIN, 16000, FORMAT_S16_MONO);
                in_rate = 16000;
            }
            LOG("audio: microphone -> 0x%08x", (unsigned)h);
            if (h < 0) {
                g_mic_on = false; // do not retry for this session
                continue;
            }
        }
        int rc = p_in_input(h, in);
        if (rc < 0) {
            // Failing for 1 s (the headset switched off and on, for example): open it again.
            if (++failures >= 200 && p_in_close) {
                LOG("audio: microphone input -> 0x%08x for 1 s, opening it again", (unsigned)rc);
                p_in_close(h);
                h = -1;
                failures = 0;
            }
            sceKernelUsleep(5000);
            continue;
        }
        failures = 0;
        // 16 kHz fallback: linear interpolation to 48 kHz (the rate announced to ALVR).
        for (uint32_t i = 0; i < GRAIN; i++) {
            if (in_rate == RATE) {
                out[out_len++] = in[i];
            } else {
                int a = in[i], b = i + 1 < GRAIN ? in[i + 1] : in[i];
                out[out_len++] = (int16_t)a;
                out[out_len++] = (int16_t)((2 * a + b) / 3);
                out[out_len++] = (int16_t)((a + 2 * b) / 3);
            }
            if (out_len >= CHUNK) {
                if (g_mic_sink)
                    g_mic_sink((const uint8_t *)out, CHUNK * 2);
                memmove(out, out + CHUNK, (out_len - CHUNK) * 2);
                out_len -= CHUNK;
            }
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------------------
// Public API

bool audio_init(int audioout_module, int audioin_module, int user_id, AudioMicSink mic_sink)
{
    p_out_init = (AudioOutInitFn)resolve(audioout_module, "sceAudioOutInit");
    p_out_open = (AudioOutOpenFn)resolve(audioout_module, "sceAudioOutOpen");
    p_out_output = (AudioOutOutputFn)resolve(audioout_module, "sceAudioOutOutput");
    p_in_open = (AudioInOpenFn)resolve(audioin_module, "sceAudioInOpen");
    p_in_input = (AudioInInputFn)resolve(audioin_module, "sceAudioInInput");
    p_in_close = (AudioInCloseFn)resolve(audioin_module, "sceAudioInClose");
    g_user_id = user_id;
    g_mic_sink = mic_sink;
    pthread_t t;
    if (p_out_init && p_out_open && p_out_output) {
        int rc = p_out_init();
        LOG("audio: sceAudioOutInit -> 0x%08x", (unsigned)rc);
        pthread_create(&t, nullptr, playback_thread, nullptr);
    }
    if (p_in_open && p_in_input)
        pthread_create(&t, nullptr, mic_thread, nullptr);
    return p_out_open != nullptr;
}

void audio_start_stream(uint32_t game_sample_rate, bool microphone)
{
    pthread_mutex_lock(&g_lock);
    g_ring_read = g_ring_count = 0;
    g_playing = false;
    g_game_on = game_sample_rate == RATE;
    pthread_mutex_unlock(&g_lock);
    if (game_sample_rate && game_sample_rate != RATE)
        LOG("audio: game audio at %u Hz is not supported (48000 only), set the PC's output device to 48 kHz",
            game_sample_rate);
    g_mic_on = microphone && p_in_open;
    LOG("audio: stream started, game audio %s, microphone %s", g_game_on ? "on" : "off", g_mic_on ? "on" : "off");
}

void audio_stop_stream()
{
    pthread_mutex_lock(&g_lock);
    g_game_on = false;
    g_playing = false;
    g_ring_read = g_ring_count = 0;
    pthread_mutex_unlock(&g_lock);
    g_mic_on = false;
}
