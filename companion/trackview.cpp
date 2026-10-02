// ALVR PS4 Tracking Viewer: a PC window showing what the PS4 app tracks (the headset, the PS
// Moves, the DualShock 4 and the PS Camera) on the lobby's floor grid, drawn by the lobby's own
// software renderer (client/src/lobby.cpp), with a camera turned with the mouse. The console
// sends its tracking state while the window says hello to it (client/src/trackview_proto.h).
// What it shows can be recorded to a .psvrdata file and played back.
// Portable Windows .exe, built by tools/build_companion.sh.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _WIN32_WINNT 0x0A00 // Windows 10 (per-monitor DPI)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include <initializer_list>
#include <vector>

#include "lobby.h"
#include "move.h"
#include "pad.h"
#include "resource.h"
#include "trackview_proto.h"

static const wchar_t APP_TITLE[] = L"ALVR PS4 Tracking Viewer";
static const float VIEW_FOV_Y = 0.9f;           // vertical field of view, radians
static const uint64_t STALE_US = 2000000;       // no state for this long: the devices turn grey
static const float DEFAULT_FLOOR_Y = -1.0f;     // before the first state: camera 1 m above the floor
static const float DEFAULT_CENTER_Z = 2.0f;

static HINSTANCE g_inst;
static HWND g_wnd;
static HDC g_dib_dc;
static HBITMAP g_dib;
static uint32_t *g_pixels;
static int g_fb_w, g_fb_h, g_client_w, g_client_h;

// Orbit camera around a target point.
static struct {
    Vec3 target;
    float yaw, pitch, dist;
} g_cam;
static int g_drag;             // 0 none, 1 orbit, 2 pan
static POINT g_drag_last;

// Connection.
static SOCKET g_sock = INVALID_SOCKET;
static sockaddr_in g_console;
static wchar_t g_console_text[64];
static bool g_connected_once;  // a console was chosen
static bool g_test_mode;       // "testing" as the IP: simulated devices
static uint64_t g_last_hello_us, g_last_state_us;
static uint32_t g_last_seq;
static uint16_t g_other_version; // the console speaks another version (0: none seen)
static LobbyView g_state;        // tracked part of the view (last state)
static bool g_have_state;

static uint64_t now_us()
{
    static LARGE_INTEGER freq;
    if (!freq.QuadPart)
        QueryPerformanceFrequency(&freq);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (uint64_t)(c.QuadPart / (double)freq.QuadPart * 1e6);
}

static Quat qmul(Quat a, Quat b)
{
    return Quat{a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
static Quat qaxis(Vec3 axis, float angle)
{
    const float s = sinf(angle * 0.5f);
    return Quat{axis.x * s, axis.y * s, axis.z * s, cosf(angle * 0.5f)};
}
static Quat qyaw(float a) { return qaxis(v3(0, 1, 0), a); }
static Quat qpitch(float a) { return qaxis(v3(1, 0, 0), a); }
static Quat qroll(float a) { return qaxis(v3(0, 0, 1), a); }

static Quat camera_rot() { return qmul(qyaw(g_cam.yaw), qpitch(g_cam.pitch)); }

// ---- Settings next to the .exe (portable) ----

static void ini_path(wchar_t *out, DWORD cap)
{
    DWORD n = GetModuleFileNameW(nullptr, out, cap);
    wchar_t *dot = wcsrchr(out, L'.');
    if (n == 0 || !dot || (size_t)(dot - out) + 5 >= cap) {
        out[0] = 0;
        return;
    }
    wcscpy(dot, L".ini");
}

static void load_last_ip(wchar_t *out, DWORD cap)
{
    wchar_t path[MAX_PATH];
    ini_path(path, MAX_PATH);
    out[0] = 0;
    if (path[0])
        GetPrivateProfileStringW(L"connection", L"ip", L"", out, cap, path);
}

static void save_last_ip(const wchar_t *ip)
{
    wchar_t path[MAX_PATH];
    ini_path(path, MAX_PATH);
    if (path[0])
        WritePrivateProfileStringW(L"connection", L"ip", ip, path);
}

// ---- Camera ----

static float floor_y() { return g_have_state ? g_state.floor_y : DEFAULT_FLOOR_Y; }

static void reset_camera()
{
    const float cx = g_have_state ? g_state.center_x : 0.0f, cz = g_have_state ? g_state.center_z : DEFAULT_CENTER_Z;
    // Near the user, the PS Camera still in view, seen from behind the user's right.
    g_cam.target = v3(cx * 0.7f, floor_y() + 1.1f, cz * 0.7f);
    g_cam.yaw = 0.75f;
    g_cam.pitch = -0.38f;
    const float d = sqrtf(cx * cx + cz * cz);
    g_cam.dist = d > 2.2f ? d : 2.2f;
}

// ---- Network ----

static void record_packet(const uint8_t *packet, int size, uint64_t now);

static void send_hello()
{
    uint8_t buf[16];
    const int n = trackview_pack_hello(buf, sizeof(buf));
    sendto(g_sock, (const char *)buf, n, 0, (const sockaddr *)&g_console, sizeof(g_console));
}

static void net_update(uint64_t now)
{
    if (g_test_mode || !g_connected_once || g_sock == INVALID_SOCKET)
        return;
    if (now - g_last_hello_us >= TRACKVIEW_HELLO_PERIOD_US) {
        g_last_hello_us = now;
        send_hello();
    }
    for (;;) {
        uint8_t buf[TRACKVIEW_PACKET_MAX + 64];
        sockaddr_in from;
        int len = sizeof(from);
        const int n = recvfrom(g_sock, (char *)buf, sizeof(buf), 0, (sockaddr *)&from, &len);
        if (n <= 0)
            break;
        if (from.sin_addr.s_addr != g_console.sin_addr.s_addr)
            continue;
        static LobbyView incoming;
        incoming = g_state;
        uint32_t seq;
        uint16_t version;
        if (!trackview_unpack_state(buf, n, &incoming, &seq, &version)) {
            if (version)
                g_other_version = version;
            continue;
        }
        // Older than the last one (UDP reordering), unless the app restarted meanwhile.
        if (g_have_state && (int32_t)(seq - g_last_seq) <= 0 && now - g_last_state_us < STALE_US)
            continue;
        const bool first = !g_have_state;
        g_state = incoming;
        g_have_state = true;
        g_last_seq = seq;
        g_last_state_us = now;
        g_other_version = 0;
        record_packet(buf, n, now);
        if (first)
            reset_camera(); // framed on the real floor and play space
    }
}

static bool connect_to(const wchar_t *text)
{
    wchar_t ip[64];
    // Trim spaces.
    while (*text == L' ' || *text == L'\t')
        text++;
    wcsncpy(ip, text, 63);
    ip[63] = 0;
    for (size_t n = wcslen(ip); n > 0 && (ip[n - 1] == L' ' || ip[n - 1] == L'\t'); n--)
        ip[n - 1] = 0;

    const bool test = _wcsicmp(ip, L"testing") == 0;
    sockaddr_in addr{};
    if (!test) {
        char host[64];
        WideCharToMultiByte(CP_UTF8, 0, ip, -1, host, sizeof(host), nullptr, nullptr);
        addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        if (!host[0] || getaddrinfo(host, nullptr, &hints, &res) != 0 || !res)
            return false;
        addr = *(const sockaddr_in *)res->ai_addr;
        freeaddrinfo(res);
        addr.sin_port = htons(TRACKVIEW_PORT);
        save_last_ip(ip);
    }
    g_test_mode = test;
    g_console = addr;
    wcscpy(g_console_text, ip);
    g_connected_once = true;
    g_have_state = false;
    g_other_version = 0;
    memset(&g_state, 0, sizeof(g_state));
    g_last_hello_us = 0;
    reset_camera();
    return true;
}

// ---- Test mode: simulated devices with random buttons ----

static uint32_t g_rng = 0x12345678u;
static uint32_t rnd()
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}
static float frand() { return (rnd() >> 8) / 16777216.0f; }

// Random buttons held for a random time: often none, else one (sometimes two).
struct SimButtons {
    double next;
    uint32_t bits;
    float trigger_target, a, b; // analog targets
    bool touch;
};

static uint32_t pick(const uint32_t *pool, int n)
{
    uint32_t bits = 0;
    if (frand() < 0.45f)
        return 0;
    bits |= pool[rnd() % n];
    if (frand() < 0.15f)
        bits |= pool[rnd() % n];
    return bits;
}

static void sim_step(SimButtons &s, double t, const uint32_t *pool, int n)
{
    if (t < s.next)
        return;
    s.next = t + 0.35 + frand() * 0.9;
    s.bits = pick(pool, n);
    const float r = frand();
    s.trigger_target = r < 0.55f ? 0.0f : r < 0.8f ? 0.5f : 1.0f;
    s.a = frand();
    s.b = frand();
    s.touch = frand() < 0.3f;
}

static float approach(float v, float target, float k) { return v + (target - v) * k; }

static void simulate(double t, LobbyView *v)
{
    static SimButtons moves[2], pads[2];
    static float trig[2], l2[2], r2[2];
    static double rumble_until[2];
    static float rumble_large[2], rumble_small[2];
    static Vec3 last_move_pos[2];

    const float floor = -1.05f, cx = 0.05f, cz = 2.1f;
    v->floor_y = floor;
    v->center_x = cx;
    v->center_z = cz;
    v->grid_visible = true;

    // Headset: the user faces the camera (-Z), sways and looks around; lost 2.5 s every 12 s
    // (its lights turn grey).
    const float ft = (float)t;
    const float head_yaw = 0.5f * sinf(0.31f * ft) + 0.25f * sinf(0.83f * ft);
    const Quat head_q = qmul(qmul(qyaw(head_yaw), qpitch(-0.1f + 0.12f * sinf(0.47f * ft))), qroll(0.05f * sinf(0.6f * ft)));
    const Vec3 head = v3(cx + 0.18f * sinf(0.23f * ft), floor + 1.62f + 0.02f * sinf(1.9f * ft), cz + 0.12f * sinf(0.17f * ft));
    v->headset.visible = true;
    v->headset.pos = head;
    v->headset.rot = head_q;
    v->headset.tracked = fmod(t, 12.0) < 9.5;

    // PS Moves in both hands, waving, with quick swings now and then; each loses the camera
    // for 2 s every 17 s (held where it was, grey).
    static const uint32_t move_pool[] = {MOVE_BUTTON_SQUARE, MOVE_BUTTON_TRIANGLE, MOVE_BUTTON_CROSS,
                                         MOVE_BUTTON_CIRCLE, MOVE_BUTTON_MOVE,     MOVE_BUTTON_START};
    const Quat body = qyaw(head_yaw * 0.6f);
    for (int i = 0; i < 2; i++) {
        SimButtons &s = moves[i];
        sim_step(s, t, move_pool, 6);
        const float side = i == 0 ? -1.0f : 1.0f, ph = i * 1.7f;
        const bool swinging = fmod(t + i * 4.0, 12.0) < 3.0;
        const float amp = swinging ? 0.28f : 0.10f, speed = swinging ? 9.0f : 1.3f;
        const Vec3 local = v3(side * 0.22f + amp * 0.6f * sinf(speed * ft + ph), -0.38f + amp * sinf(speed * 0.7f * ft + 1.0f + ph),
                              -0.32f + 0.12f * sinf(1.1f * ft + ph));
        LobbyView::Controller &c = v->controllers[i];
        c.visible = true;
        c.tracked = fmod(t + i * 7.0, 17.0) >= 2.0;
        if (c.tracked || last_move_pos[i].z == 0.0f)
            last_move_pos[i] = head + rotate(body, local);
        c.pos = last_move_pos[i];
        c.rot = qmul(qmul(body, qyaw(side * -0.25f + 0.3f * sinf(0.9f * ft + ph))),
                     qmul(qpitch(0.35f + (swinging ? 0.9f : 0.4f) * sinf(speed * 0.8f * ft + ph)), qroll(0.3f * sinf(0.7f * ft + ph))));
        c.rgb = i == 0 ? 0xff00ff : 0x00ffff;
        c.hand_letter = i == 0 ? 'L' : 'R';
        c.buttons = (uint16_t)s.bits;
        trig[i] = approach(trig[i], s.trigger_target, 0.15f);
        c.trigger = trig[i] < 0.01f ? 0.0f : trig[i];
        if (c.trigger > 0.16f)
            c.buttons |= MOVE_BUTTON_T;
        c.pad_touch = s.touch;
        c.pad_x = 0.8f * cosf(2.0f * ft + s.a * 6.28f);
        c.pad_y = 0.8f * sinf(2.0f * ft + s.a * 6.28f);
        c.pad_click = s.touch && s.b < 0.3f;
        c.battery = i == 0 ? 0.6f : 0.8f;
        c.charging = i == 1 && fmod(t, 30.0) < 10.0;
    }

    // DualShock 4 lying on a table in front, light bar towards the camera, turning slowly;
    // another user's one shown floating (not tracked), as in the lobby.
    static const uint32_t pad_pool[] = {PAD_BUTTON_CROSS, PAD_BUTTON_CIRCLE, PAD_BUTTON_SQUARE, PAD_BUTTON_TRIANGLE,
                                        PAD_BUTTON_UP,    PAD_BUTTON_DOWN,   PAD_BUTTON_LEFT,   PAD_BUTTON_RIGHT,
                                        PAD_BUTTON_L1,    PAD_BUTTON_R1,     PAD_BUTTON_L3,     PAD_BUTTON_R3,
                                        PAD_BUTTON_OPTIONS, PAD_BUTTON_TOUCH_PAD};
    for (int k = 0; k < 2; k++) {
        SimButtons &s = pads[k];
        sim_step(s, t, pad_pool, 14);
        LobbyView::Pad &p = v->pads[k];
        p.visible = true;
        if (k == 0) {
            p.pos = v3(cx + 0.45f, floor + 0.75f, cz - 0.75f);
            p.rot = qyaw(0.6f * sinf(0.2f * ft));
            p.tracked = true;
            p.rgb = 0x0000ff;
            p.label = 0;
        } else {
            const float tilt = 0.70f;
            p.pos = v3(cx + (k - 0.5f * (LOBBY_PADS - 1)) * 0.25f, floor + 1.0f, cz - 0.55f);
            p.rot = qpitch(tilt);
            p.floating = true;
            p.rgb = 0xff0000;
            p.label = '2';
        }
        p.buttons = s.bits;
        const float ang = 1.5f * ft + k;
        const bool sticks = s.a < 0.6f;
        p.lx = sticks ? cosf(ang) : 0.0f;
        p.ly = sticks ? sinf(ang) : 0.0f;
        p.rx = s.b < 0.5f ? 0.7f * sinf(2.3f * ft) : 0.0f;
        p.ry = s.b < 0.5f ? 0.7f * cosf(1.7f * ft) : 0.0f;
        l2[k] = approach(l2[k], s.a > 0.7f ? s.trigger_target : 0.0f, 0.15f);
        r2[k] = approach(r2[k], s.b > 0.6f ? s.trigger_target : 0.0f, 0.15f);
        p.l2 = l2[k];
        p.r2 = r2[k];
        if (p.l2 > 0.05f)
            p.buttons |= PAD_BUTTON_L2;
        if (p.r2 > 0.05f)
            p.buttons |= PAD_BUTTON_R2;
        p.touch[0] = s.touch;
        p.touch_x[0] = 0.5f + 0.4f * cosf(2.5f * ft);
        p.touch_y[0] = 0.5f + 0.4f * sinf(2.5f * ft);
        p.touch[1] = s.touch && s.a > 0.5f;
        p.touch_x[1] = 0.5f + 0.4f * cosf(2.5f * ft + 3.14f);
        p.touch_y[1] = 0.5f + 0.4f * sinf(2.5f * ft + 3.14f);
        if (t >= rumble_until[k] + 1.5 && frand() < 0.01f) { // a rumble pulse now and then
            rumble_until[k] = t + 0.4 + frand();
            rumble_large[k] = frand();
            rumble_small[k] = frand();
        }
        const bool on = t < rumble_until[k];
        p.rumble_large = on ? rumble_large[k] : 0.0f;
        p.rumble_small = on ? rumble_small[k] : 0.0f;
        p.battery = k == 0 ? 0.4f : 1.0f;
        p.charging = k == 1;
    }
}

// ---- Recording and playback (.psvrdata) ----

// A clip: the states recorded (or loaded), each kept as its wire packet (trackview_proto.h)
// with its time from the start of the clip.
// .psvrdata file: "PSVRDATA", u32 file format version, u32 wire version (TRACKVIEW_VERSION),
// u32 frame count, u32 duration (ms); then per frame: u32 time (ms), u16 size, the packet.
static const char PSVRDATA_MAGIC[8] = {'P', 'S', 'V', 'R', 'D', 'A', 'T', 'A'};
static const uint32_t PSVRDATA_VERSION = 1;
static const size_t CLIP_MAX_BYTES = 512u << 20; // about 2 h 45 at 60 states a second

struct ClipFrame {
    uint32_t t_ms, offset;
    uint16_t size;
};
static std::vector<ClipFrame> g_clip;
static std::vector<uint8_t> g_clip_bytes;
static wchar_t g_clip_name[MAX_PATH]; // file name in the title ("" for a recording not saved)
static bool g_clip_unsaved;
static bool g_recording;
static uint64_t g_rec_start_us;
static uint32_t g_rec_seq;
static bool g_playback, g_playing, g_seeking;
static double g_play_ms;
static uint64_t g_play_last_us;
static LobbyView g_play_view;
static bool g_play_valid;

static uint32_t clip_duration_ms() { return g_clip.empty() ? 0 : g_clip.back().t_ms; }

static void clip_append(const uint8_t *packet, int size, uint32_t t_ms)
{
    if (g_clip_bytes.size() + size > CLIP_MAX_BYTES) {
        g_recording = false;
        return;
    }
    g_clip.push_back(ClipFrame{t_ms, (uint32_t)g_clip_bytes.size(), (uint16_t)size});
    g_clip_bytes.insert(g_clip_bytes.end(), packet, packet + size);
}

static void record_packet(const uint8_t *packet, int size, uint64_t now)
{
    if (g_recording)
        clip_append(packet, size, (uint32_t)((now - g_rec_start_us) / 1000));
}

static void record_view(const LobbyView *v, uint64_t now)
{
    if (!g_recording)
        return;
    uint8_t buf[TRACKVIEW_PACKET_MAX];
    const int n = trackview_pack_state(v, g_rec_seq++, buf, sizeof(buf));
    if (n > 0)
        record_packet(buf, n, now);
}

// The state at the playhead into g_play_view.
static void playback_seek(double ms)
{
    const uint32_t dur = clip_duration_ms();
    g_play_ms = ms < 0 ? 0 : ms > dur ? dur : ms;
    g_play_valid = false;
    if (g_clip.empty())
        return;
    size_t lo = 0, hi = g_clip.size(); // last frame with t <= playhead
    while (hi - lo > 1) {
        const size_t mid = (lo + hi) / 2;
        if (g_clip[mid].t_ms <= g_play_ms)
            lo = mid;
        else
            hi = mid;
    }
    const ClipFrame &f = g_clip[lo];
    memset(&g_play_view, 0, sizeof(g_play_view));
    uint32_t seq;
    uint16_t version;
    g_play_valid = trackview_unpack_state(&g_clip_bytes[f.offset], f.size, &g_play_view, &seq, &version);
}

static void playback_update(uint64_t now)
{
    if (g_playback && g_playing && !g_seeking) {
        double ms = g_play_ms + (now - g_play_last_us) / 1000.0;
        if (ms >= clip_duration_ms()) { // stops on the last frame
            ms = clip_duration_ms();
            g_playing = false;
        }
        playback_seek(ms);
    }
    g_play_last_us = now;
}

static bool write_clip(const wchar_t *path)
{
    FILE *f = _wfopen(path, L"wb");
    if (!f)
        return false;
    const uint32_t head[4] = {PSVRDATA_VERSION, TRACKVIEW_VERSION, (uint32_t)g_clip.size(), clip_duration_ms()};
    bool ok = fwrite(PSVRDATA_MAGIC, 8, 1, f) == 1 && fwrite(head, sizeof(head), 1, f) == 1;
    for (size_t i = 0; ok && i < g_clip.size(); i++) {
        const ClipFrame &c = g_clip[i];
        ok = fwrite(&c.t_ms, 4, 1, f) == 1 && fwrite(&c.size, 2, 1, f) == 1 &&
             fwrite(&g_clip_bytes[c.offset], c.size, 1, f) == 1;
    }
    ok = fclose(f) == 0 && ok;
    return ok;
}

// Loads a .psvrdata into the clip; on failure the clip is left as it was and *why says why.
static bool read_clip(const wchar_t *path, const wchar_t **why)
{
    *why = L"The file could not be read.";
    FILE *f = _wfopen(path, L"rb");
    if (!f)
        return false;
    char magic[8];
    uint32_t head[4];
    std::vector<ClipFrame> clip;
    std::vector<uint8_t> bytes;
    bool ok = fread(magic, 8, 1, f) == 1 && fread(head, sizeof(head), 1, f) == 1 && memcmp(magic, PSVRDATA_MAGIC, 8) == 0;
    if (!ok)
        *why = L"This is not an ALVR PS4 tracking recording (.psvrdata).";
    else if (head[0] != PSVRDATA_VERSION || head[1] != TRACKVIEW_VERSION) {
        *why = L"This recording was made by another version of ALVR PS4 Tracking Viewer.";
        ok = false;
    }
    uint32_t last_t = 0;
    for (uint32_t i = 0; ok && i < head[2]; i++) {
        uint32_t t;
        uint16_t size;
        uint8_t buf[TRACKVIEW_PACKET_MAX];
        ok = fread(&t, 4, 1, f) == 1 && fread(&size, 2, 1, f) == 1 && size <= sizeof(buf) && t >= last_t &&
             fread(buf, size, 1, f) == 1;
        if (!ok) {
            *why = L"The recording is damaged or incomplete.";
            break;
        }
        last_t = t;
        clip.push_back(ClipFrame{t, (uint32_t)bytes.size(), size});
        bytes.insert(bytes.end(), buf, buf + size);
    }
    fclose(f);
    if (!ok)
        return false;
    if (clip.empty()) {
        *why = L"The recording is empty.";
        return false;
    }
    g_clip.swap(clip);
    g_clip_bytes.swap(bytes);
    return true;
}

static const wchar_t *file_part(const wchar_t *path)
{
    const wchar_t *a = wcsrchr(path, L'\\'), *b = wcsrchr(path, L'/');
    const wchar_t *p = a > b ? a : b;
    return p ? p + 1 : path;
}

static const wchar_t FILE_FILTER[] = L"ALVR PS4 tracking recordings (*.psvrdata)\0*.psvrdata\0All files\0*.*\0";

// Asks where to save the clip and saves it. False if cancelled or failed.
static bool save_clip_as()
{
    if (g_clip.empty())
        return false;
    wchar_t path[MAX_PATH];
    if (g_clip_name[0]) {
        wcsncpy(path, g_clip_name, MAX_PATH - 1);
        path[MAX_PATH - 1] = 0;
    } else {
        SYSTEMTIME st;
        GetLocalTime(&st);
        swprintf(path, MAX_PATH, L"tracking-%04d%02d%02d-%02d%02d%02d.psvrdata", st.wYear, st.wMonth, st.wDay, st.wHour,
                 st.wMinute, st.wSecond);
    }
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_wnd;
    ofn.lpstrFilter = FILE_FILTER;
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"psvrdata";
    ofn.lpstrTitle = L"Save the tracking recording";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&ofn))
        return false;
    if (!write_clip(path)) {
        MessageBoxW(g_wnd, L"The recording could not be saved there.", APP_TITLE, MB_ICONERROR);
        return false;
    }
    wcscpy(g_clip_name, file_part(path));
    g_clip_unsaved = false;
    return true;
}

// Before the clip is replaced or the window closed: an unsaved recording can be saved first.
// False: the user cancelled.
static bool clip_can_go()
{
    if (!g_clip_unsaved || g_clip.empty())
        return true;
    const int r = MessageBoxW(g_wnd, L"Save the recording first?", APP_TITLE, MB_YESNOCANCEL | MB_ICONQUESTION);
    return r == IDNO || (r == IDYES && save_clip_as());
}

static void go_live()
{
    g_playback = false;
    g_playing = false;
}

static void start_playback()
{
    if (g_clip.empty() || g_recording)
        return;
    g_play_last_us = now_us(); // the clock runs from now
    if (!g_playback) {
        g_playback = true;
        if (g_play_ms >= clip_duration_ms())
            g_play_ms = 0;
        playback_seek(g_play_ms);
    }
}

static void load_clip(const wchar_t *path)
{
    if (!clip_can_go())
        return;
    const wchar_t *why;
    if (!read_clip(path, &why)) {
        MessageBoxW(g_wnd, why, APP_TITLE, MB_ICONWARNING);
        return;
    }
    g_recording = false;
    wcsncpy(g_clip_name, file_part(path), MAX_PATH - 1);
    g_clip_unsaved = false;
    g_play_ms = 0;
    g_playback = false;
    start_playback();
    g_playing = true;
    if (g_play_valid) { // framed on the recorded play space
        const bool had = g_have_state;
        const LobbyView keep = g_state;
        g_state = g_play_view;
        g_have_state = true;
        reset_camera();
        g_state = keep;
        g_have_state = had;
    }
}

static void open_clip()
{
    wchar_t path[MAX_PATH] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_wnd;
    ofn.lpstrFilter = FILE_FILTER;
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Load a tracking recording";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&ofn))
        load_clip(path);
}

static void toggle_record(uint64_t now)
{
    if (g_recording) {
        g_recording = false;
        return;
    }
    if (!clip_can_go())
        return;
    go_live();
    g_clip.clear();
    g_clip_bytes.clear();
    g_clip_name[0] = 0;
    g_clip_unsaved = true;
    g_rec_start_us = now;
    g_rec_seq = 0;
    g_play_ms = 0;
    g_recording = true;
}

static void toggle_play()
{
    if (g_recording || g_clip.empty())
        return;
    if (!g_playback) {
        start_playback();
        g_playing = true;
    } else if (g_playing) {
        g_playing = false;
    } else {
        if (g_play_ms >= clip_duration_ms())
            playback_seek(0);
        g_play_last_us = now_us();
        g_playing = true;
    }
}

// Playhead to `ms`, from `relative` ? the playhead : the start.
static void seek_to(double ms, bool relative)
{
    if (g_recording || g_clip.empty())
        return;
    start_playback();
    playback_seek(relative ? g_play_ms + ms : ms);
}

// ---- Rendering ----

static void resize_framebuffer(int w, int h)
{
    if (w < 1)
        w = 1;
    if (h < 1)
        h = 1;
    g_client_w = w;
    g_client_h = h;
    const int fw = (w + 1) & ~1; // the renderer clears two pixels at a time
    if (fw == g_fb_w && h == g_fb_h)
        return;
    if (g_dib) {
        DeleteObject(g_dib);
        g_dib = nullptr;
    }
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = fw;
    bi.bmiHeader.biHeight = -h; // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void *bits = nullptr;
    g_dib = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    g_pixels = (uint32_t *)bits;
    g_fb_w = g_dib ? fw : 0;
    g_fb_h = g_dib ? h : 0;
    if (!g_dib_dc)
        g_dib_dc = CreateCompatibleDC(nullptr);
    if (g_dib)
        SelectObject(g_dib_dc, g_dib);
}

static void render(uint64_t now)
{
    if (!g_pixels)
        return;
    static LobbyView v;
    memset(&v, 0, sizeof(v));
    const double t = now / 1e6;
    if (g_playback) {
        if (g_play_valid)
            v = g_play_view;
    } else if (g_have_state) {
        v = g_state;
        if (!g_test_mode && now - g_last_state_us > STALE_US) { // the console stopped answering: last poses, grey
            v.headset.tracked = false;
            for (auto &c : v.controllers)
                c.tracked = false;
            for (auto &p : v.pads)
                p.tracked = false;
        }
    } else {
        v.floor_y = DEFAULT_FLOOR_Y;
        v.center_z = DEFAULT_CENTER_Z;
        v.grid_visible = true;
    }
    // The lobby's surroundings without the text: grid, PS Camera, devices.
    v.brightness = 1.0f;
    v.black = 0.0f;
    v.beacon = false;
    v.info[0] = nullptr;
    v.panel = nullptr;
    v.overlay_text = nullptr;
    v.time_s = (float)fmod(t, 3600.0);
    const Quat q = camera_rot();
    v.eye_rot[0] = q;
    v.eye_pos[0] = g_cam.target - rotate(q, v3(0, 0, -1)) * g_cam.dist;
    const float ty = tanf(VIEW_FOV_Y * 0.5f), tx = ty * g_fb_w / (float)g_fb_h;
    v.fov[0] = EyeFov{tx, tx, ty, ty};
    lobby_render_eye(g_pixels, g_fb_w, g_fb_h, g_fb_w, &v, 0);
}

static void update_title(uint64_t now)
{
    wchar_t title[MAX_PATH + 160];
    const int cap = sizeof(title) / sizeof(title[0]);
    if (g_playback)
        swprintf(title, cap, L"%ls - playing %ls", APP_TITLE, g_clip_name[0] ? g_clip_name : L"the recording");
    else if (g_test_mode)
        swprintf(title, cap, L"%ls - test mode", APP_TITLE);
    else if (!g_connected_once)
        swprintf(title, cap, L"%ls - not connected (F2 to connect)", APP_TITLE);
    else if (g_other_version)
        swprintf(title, cap, L"%ls - %ls runs another version of ALVR PS4", APP_TITLE, g_console_text);
    else if (g_have_state && now - g_last_state_us < STALE_US)
        swprintf(title, cap, L"%ls - %ls", APP_TITLE, g_console_text);
    else
        swprintf(title, cap, L"%ls - waiting for %ls (is ALVR PS4 running?)", APP_TITLE, g_console_text);
    static wchar_t shown[MAX_PATH + 160];
    if (wcscmp(title, shown) != 0) {
        wcscpy(shown, title);
        SetWindowTextW(g_wnd, title);
    }
}

// ---- Transport bar ----

static HWND g_btn_record, g_btn_play, g_btn_live, g_btn_load, g_btn_save, g_seek, g_time;
static HFONT g_font;
static UINT g_dpi = 96;
static int g_bar_h;

static int px(int v) { return MulDiv(v, (int)g_dpi, 96); }

static HWND make_button(const wchar_t *text, int id)
{
    HWND b = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 10, 10, g_wnd,
                             (HMENU)(INT_PTR)id, g_inst, nullptr);
    return b;
}

// The slider goes straight to where it is clicked and follows the mouse while dragged, like a
// video player's (the trackbar's own click on its channel moves it by pages of a fifth of
// the recording, which made it jump).
static LRESULT CALLBACK seek_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR)
{
    auto seek_at = [&](int x) {
        RECT ch, th;
        SendMessageW(w, TBM_GETCHANNELRECT, 0, (LPARAM)&ch);
        SendMessageW(w, TBM_GETTHUMBRECT, 0, (LPARAM)&th);
        const int half = (th.right - th.left) / 2, x0 = ch.left + half, x1 = ch.right - half;
        double u = x1 > x0 ? (double)(x - x0) / (x1 - x0) : 0.0;
        u = u < 0.0 ? 0.0 : u > 1.0 ? 1.0 : u;
        const double ms = u * clip_duration_ms();
        SendMessageW(w, TBM_SETPOS, TRUE, (LPARAM)ms);
        start_playback();
        playback_seek(ms);
        RECT view{0, 0, g_client_w, g_client_h};
        InvalidateRect(g_wnd, &view, FALSE);
    };
    switch (msg) {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        if (!g_clip.empty() && !g_recording) {
            SetCapture(w);
            g_seeking = true;
            seek_at(GET_X_LPARAM(lp));
        }
        return 0;
    case WM_MOUSEMOVE:
        if (g_seeking && GetCapture() == w)
            seek_at(GET_X_LPARAM(lp));
        return 0;
    case WM_LBUTTONUP:
        if (GetCapture() == w)
            ReleaseCapture();
        return 0;
    case WM_CAPTURECHANGED:
        if (g_seeking) { // playing goes on from there
            g_seeking = false;
            g_play_last_us = now_us();
            SetFocus(g_wnd);
        }
        return 0;
    case WM_MOUSEWHEEL: // the wheel zooms the view, it does not move the playhead
        return SendMessageW(g_wnd, msg, wp, lp);
    }
    return DefSubclassProc(w, msg, wp, lp);
}

static void create_bar()
{
    g_btn_record = make_button(L"\u25CF Record", IDM_RECORD);
    g_btn_play = make_button(L"\u25B6 Play", IDM_PLAY);
    g_btn_live = make_button(L"Live", IDM_LIVE);
    g_btn_load = make_button(L"Load...", IDM_LOAD);
    g_btn_save = make_button(L"Save...", IDM_SAVE);
    g_seek = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS, 0, 0, 10, 10,
                             g_wnd, (HMENU)(INT_PTR)IDC_SEEK, g_inst, nullptr);
    SetWindowSubclass(g_seek, seek_proc, 1, 0);
    g_time = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE, 0, 0, 10, 10, g_wnd,
                             (HMENU)(INT_PTR)IDC_TIME, g_inst, nullptr);
}

static void layout_bar(int w, int h)
{
    const UINT dpi = GetDpiForWindow(g_wnd);
    if (dpi != g_dpi || !g_font) {
        g_dpi = dpi ? dpi : 96;
        if (g_font)
            DeleteObject(g_font);
        g_font = CreateFontW(-MulDiv(9, (int)g_dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        for (HWND c : {g_btn_record, g_btn_play, g_btn_live, g_btn_load, g_btn_save, g_seek, g_time})
            SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    }
    g_bar_h = px(40);
    const int y = h - g_bar_h + px(6), bh = g_bar_h - px(12), gap = px(6);
    int x = px(8);
    const struct {
        HWND h;
        int w;
    } left[] = {{g_btn_record, 92}, {g_btn_play, 82}, {g_btn_live, 56}, {g_btn_load, 70}, {g_btn_save, 70}};
    for (auto &b : left) {
        MoveWindow(b.h, x, y, px(b.w), bh, TRUE);
        x += px(b.w) + gap;
    }
    const int time_w = px(130);
    MoveWindow(g_time, w - time_w - px(8), y, time_w, bh, TRUE);
    const int seek_w = w - time_w - px(16) - x;
    MoveWindow(g_seek, x, y, seek_w > px(40) ? seek_w : px(40), bh, TRUE);
}

static void format_time(wchar_t *out, int cap, double ms, bool tenths)
{
    const int total = (int)(ms / 100.0); // tenths of a second
    if (tenths)
        swprintf(out, cap, L"%d:%02d.%d", total / 600, total / 10 % 60, total % 10);
    else
        swprintf(out, cap, L"%d:%02d", total / 600, total / 10 % 60);
}

static void set_text_if_changed(HWND w, const wchar_t *text)
{
    wchar_t cur[64];
    GetWindowTextW(w, cur, 64);
    if (wcscmp(cur, text) != 0)
        SetWindowTextW(w, text);
}

static void update_bar(uint64_t now)
{
    const bool clip = !g_clip.empty();
    set_text_if_changed(g_btn_record, g_recording ? L"\u25A0 Stop" : L"\u25CF Record");
    set_text_if_changed(g_btn_play, g_playback && g_playing ? L"\u275A\u275A Pause" : L"\u25B6 Play");
    EnableWindow(g_btn_play, clip && !g_recording);
    EnableWindow(g_btn_live, g_playback);
    EnableWindow(g_btn_save, clip && !g_recording);
    EnableWindow(g_seek, clip && !g_recording);
    const uint32_t dur = clip_duration_ms();
    if ((uint32_t)SendMessageW(g_seek, TBM_GETRANGEMAX, 0, 0) != dur)
        SendMessageW(g_seek, TBM_SETRANGEMAX, TRUE, dur);
    if (!g_seeking && GetCapture() != g_seek)
        SendMessageW(g_seek, TBM_SETPOS, TRUE, g_recording ? dur : (LPARAM)g_play_ms);
    wchar_t text[64], a[24], b[24];
    if (g_recording) {
        format_time(a, 24, (double)(now - g_rec_start_us) / 1000.0, false);
        swprintf(text, 64, L"REC %ls", a);
    } else if (g_playback) {
        format_time(a, 24, g_play_ms, true);
        format_time(b, 24, dur, true);
        swprintf(text, 64, L"%ls / %ls", a, b);
    } else {
        swprintf(text, 64, L"Live");
    }
    set_text_if_changed(g_time, text);
}

// ---- UI ----

static INT_PTR CALLBACK ip_dialog_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM)
{
    switch (msg) {
    case WM_INITDIALOG: {
        wchar_t last[64];
        load_last_ip(last, 64);
        SetDlgItemTextW(dlg, IDC_IP, g_connected_once && !g_test_mode ? g_console_text : last);
        SendDlgItemMessageW(dlg, IDC_IP, EM_SETSEL, 0, -1);
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            wchar_t text[64];
            GetDlgItemTextW(dlg, IDC_IP, text, 64);
            if (connect_to(text)) {
                EndDialog(dlg, IDOK);
            } else {
                MessageBoxW(dlg, L"Enter the IP address shown in the ALVR PS4 lobby, for example 192.168.0.124.",
                            APP_TITLE, MB_ICONWARNING);
                SetFocus(GetDlgItem(dlg, IDC_IP));
                SendDlgItemMessageW(dlg, IDC_IP, EM_SETSEL, 0, -1);
            }
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) {
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

static void ask_ip()
{
    // Cancel keeps the window open without a console (recordings can still be played).
    DialogBoxW(g_inst, MAKEINTRESOURCEW(IDD_IP), g_wnd, ip_dialog_proc);
    if (g_playback && g_connected_once)
        go_live();
}

static void show_controls()
{
    MessageBoxW(g_wnd,
                L"Left drag: turn around\n"
                L"Right drag (or Shift + left drag): move\n"
                L"Mouse wheel: zoom\n"
                L"Double-click or R: reset the camera\n"
                L"F2: change the PS4 IP address\n"
                L"\n"
                L"Ctrl+R: record / stop\n"
                L"Space: play / pause\n"
                L"Left / Right: 5 s back / forward, Home: to the start\n"
                L"L: back to live\n"
                L"Ctrl+O: load a recording (or drop a .psvrdata file on the window)\n"
                L"Ctrl+S: save the recording",
                APP_TITLE, MB_ICONINFORMATION);
}

static void on_command(int id, uint64_t now)
{
    switch (id) {
    case IDM_CHANGE_IP: ask_ip(); break;
    case IDM_EXIT: PostMessageW(g_wnd, WM_CLOSE, 0, 0); break;
    case IDM_RESET_VIEW: reset_camera(); break;
    case IDM_CONTROLS: show_controls(); break;
    case IDM_RECORD: toggle_record(now); break;
    case IDM_PLAY: toggle_play(); break;
    case IDM_LIVE: go_live(); break;
    case IDM_LOAD: open_clip(); break;
    case IDM_SAVE:
        if (!g_recording)
            save_clip_as();
        break;
    case IDM_BACK: seek_to(-5000, true); break;
    case IDM_FORWARD: seek_to(5000, true); break;
    case IDM_START: seek_to(0, false); break;
    }
    update_bar(now);
    SetFocus(g_wnd); // keys keep going to the view after a click on a button
}

static LRESULT CALLBACK wnd_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SIZE:
        if (g_seek)
            layout_bar(LOWORD(lp), HIWORD(lp));
        resize_framebuffer(LOWORD(lp), HIWORD(lp) - g_bar_h);
        InvalidateRect(wnd, nullptr, FALSE);
        return 0;
    case WM_TIMER: {
        const uint64_t now = now_us();
        net_update(now);
        if (g_test_mode) {
            simulate(now / 1e6, &g_state);
            g_have_state = true;
            g_last_state_us = now;
            record_view(&g_state, now);
        }
        playback_update(now);
        update_bar(now);
        update_title(now);
        if (!IsIconic(wnd)) {
            RECT r{0, 0, g_client_w, g_client_h};
            InvalidateRect(wnd, &r, FALSE);
        }
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(wnd, &ps);
        render(now_us());
        if (g_dib)
            BitBlt(dc, 0, 0, g_client_w, g_client_h, g_dib_dc, 0, 0, SRCCOPY);
        RECT bar;
        GetClientRect(wnd, &bar);
        bar.top = g_client_h;
        FillRect(dc, &bar, GetSysColorBrush(COLOR_BTNFACE));
        EndPaint(wnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_CTLCOLORSTATIC:
        if ((HWND)lp == g_time) {
            HDC dc = (HDC)wp;
            SetTextColor(dc, g_recording ? RGB(200, 30, 30) : GetSysColor(COLOR_BTNTEXT));
            SetBkColor(dc, GetSysColor(COLOR_BTNFACE));
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
        }
        break;
    case WM_HSCROLL:
        if ((HWND)lp == g_seek && !g_clip.empty() && !g_recording) {
            const int code = LOWORD(wp);
            g_seeking = code == TB_THUMBTRACK;
            start_playback();
            playback_seek((double)SendMessageW(g_seek, TBM_GETPOS, 0, 0));
            if (code == TB_ENDTRACK)
                SetFocus(wnd);
            return 0;
        }
        break;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
        SetFocus(wnd);
        g_drag = msg == WM_LBUTTONDOWN && !(wp & MK_SHIFT) ? 1 : 2;
        g_drag_last = POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        SetCapture(wnd);
        return 0;
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_MBUTTONUP:
        g_drag = 0;
        ReleaseCapture();
        return 0;
    case WM_CAPTURECHANGED:
        g_drag = 0;
        return 0;
    case WM_MOUSEMOVE:
        if (g_drag) {
            const int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
            const float dx = (float)(x - g_drag_last.x), dy = (float)(y - g_drag_last.y);
            g_drag_last = POINT{x, y};
            if (g_drag == 1) {
                g_cam.yaw -= dx * 0.006f;
                g_cam.pitch -= dy * 0.006f;
                g_cam.pitch = g_cam.pitch < -1.55f ? -1.55f : g_cam.pitch > 1.55f ? 1.55f : g_cam.pitch;
            } else { // the point under the cursor follows it
                const Quat q = camera_rot();
                const float s = 2.0f * g_cam.dist * tanf(VIEW_FOV_Y * 0.5f) / (g_client_h > 0 ? g_client_h : 1);
                g_cam.target = g_cam.target - rotate(q, v3(1, 0, 0)) * (dx * s) + rotate(q, v3(0, 1, 0)) * (dy * s);
            }
            RECT r{0, 0, g_client_w, g_client_h};
            InvalidateRect(wnd, &r, FALSE);
        }
        return 0;
    case WM_MOUSEWHEEL:
        g_cam.dist *= powf(0.88f, GET_WHEEL_DELTA_WPARAM(wp) / 120.0f);
        g_cam.dist = g_cam.dist < 0.15f ? 0.15f : g_cam.dist > 40.0f ? 40.0f : g_cam.dist;
        return 0;
    case WM_LBUTTONDBLCLK:
        reset_camera();
        return 0;
    case WM_COMMAND:
        if (lp == 0 || HIWORD(wp) == BN_CLICKED) {
            on_command(LOWORD(wp), now_us());
            return 0;
        }
        break;
    case WM_DROPFILES: {
        wchar_t path[MAX_PATH];
        if (DragQueryFileW((HDROP)wp, 0, path, MAX_PATH))
            load_clip(path);
        DragFinish((HDROP)wp);
        return 0;
    }
    case WM_DPICHANGED: {
        const RECT *r = (const RECT *)lp;
        SetWindowPos(wnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_CLOSE:
        if (clip_can_go())
            DestroyWindow(wnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}

static bool ends_with_psvrdata(const wchar_t *s)
{
    const size_t n = wcslen(s);
    return n > 9 && _wcsicmp(s + n - 9, L".psvrdata") == 0;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmd_line, int show)
{
    g_inst = inst;
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_sock != INVALID_SOCKET) {
        u_long on = 1;
        ioctlsocket(g_sock, FIONBIO, &on);
        // No WSAECONNRESET from recvfrom when the console answers "port unreachable" (app not
        // running): it would hide the next states.
        BOOL report = FALSE;
        DWORD ret = 0;
        WSAIoctl(g_sock, _WSAIOW(IOC_VENDOR, 12) /* SIO_UDP_CONNRESET */, &report, sizeof(report), nullptr, 0, &ret,
                 nullptr, nullptr);
        sockaddr_in any{};
        any.sin_family = AF_INET;
        bind(g_sock, (const sockaddr *)&any, sizeof(any));
    }
    reset_camera();

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    wc.lpszMenuName = MAKEINTRESOURCEW(IDM_MAIN);
    wc.lpszClassName = L"AlvrPs4TrackingViewer";
    RegisterClassExW(&wc);

    const UINT dpi = GetDpiForSystem();
    RECT r{0, 0, MulDiv(1280, dpi, 96), MulDiv(760, dpi, 96)};
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, TRUE, 0, dpi);
    g_wnd = CreateWindowExW(WS_EX_ACCEPTFILES, wc.lpszClassName, APP_TITLE, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                            CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst,
                            nullptr);
    create_bar();
    RECT cr;
    GetClientRect(g_wnd, &cr);
    layout_bar(cr.right, cr.bottom);
    resize_framebuffer(cr.right, cr.bottom - g_bar_h);
    update_bar(now_us());
    ShowWindow(g_wnd, show);
    UpdateWindow(g_wnd);
    SetTimer(g_wnd, 1, 15, nullptr);
    HACCEL accel = LoadAcceleratorsW(inst, MAKEINTRESOURCEW(IDA_MAIN));

    // The command line can give the PS4 IP address (a shortcut) or a recording to play;
    // otherwise the address is asked.
    wchar_t arg[MAX_PATH] = L"";
    if (cmd_line) {
        wcsncpy(arg, cmd_line[0] == L'"' ? cmd_line + 1 : cmd_line, MAX_PATH - 1);
        wchar_t *quote = wcschr(arg, L'"');
        if (quote)
            *quote = 0;
    }
    if (arg[0] && ends_with_psvrdata(arg))
        load_clip(arg);
    else if (!arg[0] || !connect_to(arg))
        ask_ip();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (TranslateAcceleratorW(g_wnd, accel, &msg))
            continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (g_sock != INVALID_SOCKET)
        closesocket(g_sock);
    WSACleanup();
    return 0;
}
