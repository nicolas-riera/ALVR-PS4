// ALVR PS4 client — stage 1: base homebrew.
//
// Shows a status screen on the TV, broadcasts logs over UDP to the PC, and
// probes the system modules the later stages depend on (Hmd, VrTracker, Move,
// Camera, video decoder, audio) so we know what loads from a homebrew process.

#include <atomic>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <orbis/libkernel.h>
#include <orbis/GnmDriver.h>
#include <orbis/NetCtl.h>
#include <orbis/Sysmodule.h>

#include <orbis/SystemService.h>
#include <orbis/Pad.h>
#include <orbis/UserService.h>

#include "hmd.h"
#include "hmd_setup.h"
#include "lobby.h"
#include "settings.h"
#include "alvr_client.h"
#include "config.h"
#include "move.h"
#include "pad.h"
#include "wand.h"
#include "reproj.h"
#include "log.h"
#include "screen.h"
#include "tracker.h"
#include "audio.h"
#include "video.h"
#include "bench.h"

#define ALVR_PS4_VERSION "0.11.0"
#if ALVR_PS4_DEV
#define ALVR_PS4_TITLE "ALVR PS4 (Dev)"
#else
#define ALVR_PS4_TITLE "ALVR PS4"
#endif

static char g_ip[16] = "?";

static void read_ip()
{
    int rc = sceNetCtlInit();
    if (rc < 0) {
        LOG("sceNetCtlInit failed: 0x%08x", rc);
        return;
    }
    OrbisNetCtlInfo info;
    memset(&info, 0, sizeof(info));
    rc = sceNetCtlGetInfo(ORBIS_NET_CTL_INFO_IP_ADDRESS, &info);
    if (rc < 0) {
        LOG("sceNetCtlGetInfo(IP) failed: 0x%08x", rc);
        return;
    }
    strncpy(g_ip, info.ip_address, sizeof(g_ip) - 1);
}

struct ModuleProbe {
    const char *name;
    uint32_t sysmodule_id; // 0 = none; 0x8000xxxx = internal id
    const char *symbols[6];
    int handle;
    int found;
    int total;
};

static ModuleProbe g_probes[] = {
    {"libSceHmd", 0x00D4, {"sceHmdInitialize", "sceHmdOpen", "sceHmdGetDeviceInformation", "sceHmdGetFieldOfView", nullptr}},
    {"libSceVrTracker", 0x00ED, {"sceVrTrackerInit", "sceVrTrackerRegisterDevice", "sceVrTrackerGetResult", nullptr}},
    {"libSceMove", 0x008F, {"sceMoveInit", "sceMoveOpen", "sceMoveReadStateLatest", nullptr}},
    {"libSceMoveTracker", 0x00B1, {"sceMoveTrackerInit", nullptr}},
    {"libSceCamera", 0x8000001A, {"sceCameraOpen", "sceCameraIsAttached", nullptr}},
    {"libSceVideodec2", 0x00CF, {"sceVideodec2CreateDecoder", "sceVideodec2Decode", nullptr}},
    {"libSceAudioOut", 0x80000001, {"sceAudioOutInit", "sceAudioOutOpen", nullptr}},
    {"libSceAudioIn", 0x80000002, {"sceAudioInOpen", nullptr}},
    {"libSceHmdDistortion", 0, {nullptr}},
    {"libSceHmdReprojectionMultilayer", 0, {nullptr}},
    {"libSceCommonDialog", 0x80000018, {"sceCommonDialogInitialize", nullptr}},
    {"libSceHmdSetupDialog", 0x00EB, {"sceHmdSetupDialogInitialize", "sceHmdSetupDialogOpen", nullptr}},
    {"libSceVrServiceDialog", 0x00FD, {"sceVrServiceDialogInitialize", "sceVrServiceDialogOpen", nullptr}},
};
static const int NUM_PROBES = sizeof(g_probes) / sizeof(g_probes[0]);

static void probe_modules()
{
    LOG("--- module probe ---");
    for (int i = 0; i < NUM_PROBES; i++) {
        ModuleProbe &p = g_probes[i];
        if (p.sysmodule_id) {
            int rc = (p.sysmodule_id & 0x80000000)
                         ? (int)sceSysmoduleLoadModuleInternal((OrbisSysModuleInternal)p.sysmodule_id)
                         : sceSysmoduleLoadModule((OrbisSysModule)p.sysmodule_id);
            LOG("%-32s sysmodule 0x%x -> 0x%08x", p.name, p.sysmodule_id, (unsigned)rc);
        }
        // Apps run in a sandbox: /system is exposed as /<random word>/common/lib.
        // Loading an already-loaded module just returns its handle.
        char path[128];
        snprintf(path, sizeof(path), "/%s/common/lib/%s.sprx", sceKernelGetFsSandboxRandomWord(), p.name);
        p.handle = (int)sceKernelLoadStartModule(path, 0, nullptr, 0, nullptr, nullptr);
        if (p.handle < 0) {
            LOG("%-32s load FAILED 0x%08x (%s)", p.name, (unsigned)p.handle, path);
            continue;
        }
        LOG("%-32s loaded, handle=%d", p.name, p.handle);
        for (int j = 0; p.symbols[j]; j++) {
            void *addr = nullptr;
            int rc = sceKernelDlsym(p.handle, p.symbols[j], &addr);
            p.total++;
            if (rc == 0 && addr)
                p.found++;
            LOG("    %-28s %s %p", p.symbols[j], rc == 0 ? "ok " : "MISSING", addr);
        }
    }
    LOG("--- probe done ---");
}

static HmdState g_hmd;
static int g_user_id = -1;
static TrackerState g_tracker;
static MoveController g_moves[MOVE_MAX];
static WandEmulator g_wand_emu[MOVE_MAX];
static WandInput g_wand[MOVE_MAX];
static PadController g_pad; // DualShock 4: lobby only
// Other logged-in users: their controllers are opened and tracked too (the tracker runs two
// controllers per user), and shown in the lobby only.
struct OtherUser {
    int user;
    MoveController moves[MOVE_MAX];
    PadController pad;
};
static const int OTHER_USERS = 3;
static OtherUser g_others[OTHER_USERS];
static int g_other_count;
static ClientConfig g_config;
static bool g_floor_set = false; // cleared by a tracking reset: the tracker origin may move
static float g_floor_y = -1.4f;   // tracker-space floor height, shared with the ALVR uplink
// Headset tracking initialized: seen by the camera since the app started or since the
// last tracking reset (back from the PS menu). Until then SteamVR shows it as searching
// and the lobby is black.
static bool g_hmd_tracking_init = false;
static uint64_t g_hmd_init_after_us = 0; // sightings before this time do not count
// Play space centre (tracker space x/z): the stage origin sent to SteamVR and the lobby
// grid alignment. Set at the user's feet once the tracking is initialized, and again each
// time SteamVR connects (user's choice, instead of a recentre button: the headset position
// at SteamVR launch is the initial centre; SteamVR's own recentre does the rest). The
// floor height and the forward direction never change.
static bool g_center_set = false;
static float g_center_x = 0.0f, g_center_z = 0.0f;
static bool g_lobby_shown = false;             // the last frame was the lobby
// Lobby while streaming (hold Cross on the left PS Move or Circle on the right one): the
// ALVR session stays open with tracking, audio and microphone, but the video is not
// decoded, the buttons are not sent to SteamVR and the game's vibrations are ignored.
static std::atomic<bool> g_stream_paused{false}; // also read by the network thread (haptics)
// First launch (no height saved yet): the height wizard shows in the lobby and the PC is
// not searched for until it is confirmed.
static bool g_wizard_pending = false; // also after a settings Reset
static bool g_alvr_started = false;
static void start_alvr();
// PSVR refresh rate in use (90, or 120 for the 60 fps stream) and the stream's frame rate.
static int g_display_hz = 120;
static int stream_fps() { return g_display_hz == 90 ? 90 : 60; }

static void init_user()
{
    int rc = sceUserServiceInitialize(nullptr);
    if (rc < 0 && (unsigned)rc != 0x80960003 /* already initialized */)
        LOG("sceUserServiceInitialize -> 0x%08x", (unsigned)rc);
    rc = sceUserServiceGetInitialUser(&g_user_id);
    LOG("initial user id=0x%x (rc=0x%08x)", g_user_id, (unsigned)rc);
}

// Call after wait_for_headset: the headset is ready.
static void start_headset()
{
    if (hmd_open(g_user_id, &g_hmd))
        LOG("headset opened, handle=0x%x", g_hmd.handle);
    else
        LOG("headset open FAILED");

    tracker_set_main_user(g_user_id);
    if (tracker_start(g_probes[1].handle, g_probes[4].handle, g_hmd.handle, &g_tracker))
        LOG("tracker started, HMD registered");
    else
        LOG("tracker start FAILED");
    move_start(g_probes[2].handle, g_user_id, g_moves);
    // The DualShock 4 is tracked too (light bar), for the lobby only.
    pad_start(g_user_id, &g_pad);
    tracker_run_thread();
    if (!video_init(g_probes[5].handle))
        LOG("video decoder unavailable");
    if (!audio_init(g_probes[6].handle, g_probes[7].handle, g_user_id, alvr_send_microphone))
        LOG("audio unavailable");
}

// System service: there is no "about to close" event for apps, the system kills the
// process. Displaying through the reprojection is what lets the system restore the
// PSVR itself. Events are only logged, to learn what the system sends.
struct SystemServiceStatus {
    int32_t event_num;
    bool is_system_ui_overlaid;
    bool is_in_background_execution;
    bool is_cpu_mode7_cpu_normal;
    bool is_game_live_streaming_on_air;
    bool is_out_of_vr_play_area;
    uint8_t reserved[128];
};

struct SystemServiceEvent {
    int32_t type;
    uint8_t param[8192];
};

// OpenOrbis declares these without parameters; call them through typed pointers.
static auto const system_get_status = (int (*)(SystemServiceStatus *))sceSystemServiceGetStatus;
static auto const system_receive_event = (int (*)(SystemServiceEvent *))sceSystemServiceReceiveEvent;

static bool g_in_background = false; // PS menu or another app in front

static void poll_system_events()
{
    static SystemServiceStatus last;
    SystemServiceStatus st;
    memset(&st, 0, sizeof(st));
    if (system_get_status(&st) < 0)
        return;
    if (last.is_in_background_execution && !st.is_in_background_execution) {
        // Back from the PS menu: reset the tracking (gyro drift), as some games do.
        LOG("back from the PS menu: recalibrating the tracking");
        TrackedDevice devs[(MOVE_MAX + 1) * (1 + OTHER_USERS)];
        int n = 0;
        for (int i = 0; i < MOVE_MAX; i++)
            devs[n++] = g_moves[i].track;
        devs[n++] = g_pad.track;
        for (int k = 0; k < g_other_count; k++) {
            for (int i = 0; i < MOVE_MAX; i++)
                devs[n++] = g_others[k].moves[i].track;
            devs[n++] = g_others[k].pad.track;
        }
        tracker_recalibrate_all(devs, n);
        g_floor_set = false;
        // The headset counts as not initialized until the camera sees it again (stale
        // results from before the recalibration are ignored).
        g_hmd_tracking_init = false;
        g_hmd_init_after_us = sceKernelGetProcessTime() + 300000;
    }
    if (st.is_system_ui_overlaid != last.is_system_ui_overlaid ||
        st.is_in_background_execution != last.is_in_background_execution ||
        st.is_out_of_vr_play_area != last.is_out_of_vr_play_area)
        LOG("system status: ui_overlaid=%d background=%d out_of_vr_play_area=%d", st.is_system_ui_overlaid,
            st.is_in_background_execution, st.is_out_of_vr_play_area);
    last = st;
    g_in_background = st.is_in_background_execution;
    for (int i = 0; i < st.event_num; i++) {
        static SystemServiceEvent ev;
        if (system_receive_event(&ev) < 0)
            break;
        LOG("system event type=0x%x", ev.type);
    }
}

// Per-eye block for the compositor: tangent -> uv transform,
//   uv = tangent * scale + offset   (x right, y down),
// stored as {scale x, scale y, offset x, offset y}. Deduced from three captures
// (image size varied inversely with the scale, right edge at -0.22 as predicted).
static void compute_fov_blocks(const EyeFov fov[2], float block[2][4])
{
    for (int eye = 0; eye < 2; eye++) {
        const EyeFov &e = fov[eye];
        const float w = e.tan_left + e.tan_right, h = e.tan_up + e.tan_down;
        block[eye][0] = 1.0f / w;
        block[eye][1] = 1.0f / h;
        block[eye][2] = e.tan_left / w;
        block[eye][3] = e.tan_up / h;
    }
}

// Headset FoV per eye, as declared to the streamer in start_alvr().
static void headset_fov(EyeFov fov[2])
{
    const HmdFieldOfView &f = g_hmd.fov;
    fov[0] = EyeFov{f.tan_out, f.tan_in, f.tan_top, f.tan_bottom};
    fov[1] = EyeFov{f.tan_in, f.tan_out, f.tan_top, f.tan_bottom};
}

// Poses sent to the streamer, by tracking timestamp: a video frame carries the timestamp
// of the pose SteamVR rendered it with, and the compositor needs that pose to reproject.
struct SentPose {
    uint64_t timestamp_ns;
    ReprojPose pose;
};
static SentPose g_sent_poses[256];
static unsigned g_sent_pose_next;

static void remember_sent_pose(uint64_t timestamp_ns)
{
    const TrackerPose &tp = g_tracker.predicted_device_pose; // what SteamVR renders with
    SentPose &s = g_sent_poses[g_sent_pose_next++ % 256];
    memset(&s, 0, sizeof(s));
    s.timestamp_ns = timestamp_ns;
    s.pose.timestamp = g_tracker.predicted_timestamp;
    s.pose.orientation[0] = tp.qx;
    s.pose.orientation[1] = tp.qy;
    s.pose.orientation[2] = tp.qz;
    s.pose.orientation[3] = tp.qw;
    s.pose.position[0] = tp.px;
    s.pose.position[1] = tp.py;
    s.pose.position[2] = tp.pz;
}

static bool find_sent_pose(uint64_t timestamp_ns, ReprojPose *out)
{
    const SentPose *best = nullptr;
    uint64_t best_diff = ~0ull;
    for (const SentPose &s : g_sent_poses) {
        if (!s.timestamp_ns)
            continue;
        uint64_t diff = s.timestamp_ns > timestamp_ns ? s.timestamp_ns - timestamp_ns : timestamp_ns - s.timestamp_ns;
        if (diff < best_diff) {
            best_diff = diff;
            best = &s;
        }
    }
    if (!best || best_diff > 100000000ull) // more than 100 ms away: not ours
        return false;
    *out = best->pose;
    return true;
}

// Head for SteamVR and the lobby: centre between the eyes, orientation of the headset
// (device pose if the eye poses are not filled). Tracker space. `predicted`: the pose
// predicted over the stream latency, for SteamVR.
static void head_pose(float p[3], float q[4], bool predicted = false)
{
    const TrackerPose *e = predicted ? g_tracker.predicted_eye_pose : g_tracker.eye_pose;
    const TrackerPose &l = e[0], &r = e[1], &d = predicted ? g_tracker.predicted_device_pose : g_tracker.device_pose;
    bool eyes = l.qw * l.qw + l.qx * l.qx + l.qy * l.qy + l.qz * l.qz > 0.5f;
    p[0] = eyes ? (l.px + r.px) * 0.5f : d.px;
    p[1] = eyes ? (l.py + r.py) * 0.5f : d.py;
    p[2] = eyes ? (l.pz + r.pz) * 0.5f : d.pz;
    q[0] = eyes ? l.qx : d.qx;
    q[1] = eyes ? l.qy : d.qy;
    q[2] = eyes ? l.qz : d.qz;
    q[3] = eyes ? l.qw : d.qw;
}

// Headset not usable: never seen since the start / the last reset, or not seen by the
// camera for 3 s. SteamVR then shows it as searching; the lobby fades to black.
static bool headset_lost(uint64_t now)
{
    return !g_hmd_tracking_init || now - g_tracker.last_seen_us > TRACKER_HMD_SEARCHING_US;
}

static void set_center_at_head(const char *why)
{
    float p[3], q[4];
    head_pose(p, q);
    g_center_x = p[0];
    g_center_z = p[2];
    g_center_set = true;
    LOG("play space centre at x=%.3f z=%.3f (%s)", g_center_x, g_center_z, why);
}

// Lobby haptic ticks only while no PC is connected (then ALVR drives the motors).
static bool lobby_haptics_allowed()
{
    AlvrStatus st;
    alvr_get_status(&st);
    return st.state != ALVR_STREAMING || g_stream_paused;
}

// Once per frame, before the uplink: tracking initialization, floor, play space centre.
static void update_tracking_state(bool streaming)
{
    if (!g_hmd_tracking_init && g_tracker.status == 1 && g_tracker.last_seen_us &&
        g_tracker.last_seen_us > g_hmd_init_after_us) {
        g_hmd_tracking_init = true;
        LOG("headset tracking initialized");
        if (!g_center_set)
            set_center_at_head("default: at the user's feet");
    }
    // SteamVR connected: its initial centre is where the headset is (once the headset is
    // tracked, if it is not yet).
    static bool was_streaming = false, center_pending = false;
    if (streaming && !was_streaming && g_config.center_on_connect)
        center_pending = true;
    was_streaming = streaming;
    if (center_pending && g_hmd_tracking_init) {
        set_center_at_head("SteamVR connected");
        center_pending = false;
    }
    if (g_config.camera_height_cm > 0) {
        g_floor_y = -g_config.camera_height_cm / 100.0f;
        g_floor_set = true;
    } else if (!g_floor_set && g_hmd_tracking_init && g_tracker.position_quality == 9) {
        // Until the height is set: the default height shown in the settings, standing.
        float hp[3], hq[4];
        head_pose(hp, hq);
        g_floor_y = hp[1] - SETTINGS_DEFAULT_USER_HEIGHT_CM * SETTINGS_EYE_HEIGHT_RATIO / 100.0f;
        g_floor_set = true;
        LOG("floor estimated at y=%.3f (head y=%.3f)", g_floor_y, hp[1]);
    }
}

static unsigned g_video_shown_seq; // last video frame handed to the compositor
// Frame pacing on the compositor's pass event (false: not available, frames are shown as
// soon as they are converted, as up to 0.9.3).
static bool g_paced;
static uint64_t g_m2p_avg_us;      // motion-to-photon of the stream, averaged
// Latency the headset prediction is based on, at most: m2p is 55-65 ms while streaming,
// but climbs to 300 ms while SteamVR resends old frames (loading, paused).
static const uint64_t HEAD_PREDICTION_LATENCY_MAX_US = 70000;

// Console model for the performance overlay: the PS4 Pro has the "Neo" hardware mode
// (libkernel says so whether this app runs in it or not); the Fat and Slim models are
// not told apart.
extern "C" int sceKernelHasNeoMode(void);
extern "C" int sceKernelIsAuthenticNeo(void);
static const char *g_ps4_model = "PS4";

static void detect_ps4_model()
{
    const int has_neo = sceKernelHasNeoMode(), authentic = sceKernelIsAuthenticNeo(), neo_mode = sceKernelIsNeoMode();
    g_ps4_model = has_neo == 1 || authentic == 1 ? "PS4 Pro" : "PS4";
    LOG("console: %s (HasNeoMode %d, IsAuthenticNeo %d, IsNeoMode %d, CPU %d MHz)", g_ps4_model, has_neo, authentic,
        neo_mode, sceKernelGetCpuFrequency() / 1000000);
}

// Performance overlay (settings: "Performance overlay"), drawn by the video conversion
// into each frame, refreshed every second: console, refresh rate and bitrate;
// frame rate, latency (tracking sample to display), decoding time; frames lost for the
// display in the last second (never shown, repeated, late).
static uint64_t g_hud_start;   // start of the second being counted (0: none)
// Per shown frame, averaged: decoding (received -> decoder picture) and wait (picture ->
// submitted: conversion, then the frame waits for its decision point).
static uint64_t g_decode_lat_us, g_wait_lat_us;
static bool g_hud_shown;       // the overlay holds text

static void hud_clear()
{
    if (g_hud_shown)
        video_set_overlay(nullptr, nullptr, 0, nullptr);
    g_hud_shown = false;
    g_hud_start = 0;
}

static void update_hud(uint64_t now, bool fresh, bool repeat)
{
    if (!g_config.hud) {
        hud_clear();
        return;
    }
    uint64_t &start = g_hud_start;
    static uint64_t last_bytes;
    static unsigned frames, repeats, last_dropped, last_lost;
    static VideoPacingStats last;
    frames += fresh;
    repeats += repeat;
    if (start && now - start < 1000000)
        return;
    VideoStats vs;
    video_peek_stats(&vs);
    VideoPacingStats ps;
    video_pacing_stats(&ps);
    if (start) {
        const double secs = (now - start) / 1e6;
        // Counters can restart from 0 (a Dev video bench clears the video statistics).
        auto diff = [](unsigned cur, unsigned before) { return cur >= before ? cur - before : cur; };
        const unsigned drops = diff(ps.overflow, last.overflow) + diff(ps.trimmed, last.trimmed) +
                               diff(ps.replaced, last.replaced) + diff(vs.dropped, last_dropped);
        const unsigned late = diff(ps.late, last.late), lost = diff(vs.lost, last_lost);
        const uint64_t bytes = vs.bytes_total >= last_bytes ? vs.bytes_total - last_bytes : vs.bytes_total;
        // Short lines: at a legible size each character spans ~1.3 degrees of the view.
        static char lines[4][48];
        snprintf(lines[0], sizeof(lines[0]), "%s | %d Hz | %.0f Mbps", g_ps4_model, g_display_hz,
                 bytes * 8.0 / 1e6 / secs);
        snprintf(lines[1], sizeof(lines[1]), "FPS %.1f | Latency %.0f ms", frames / secs, g_m2p_avg_us / 1000.0);
        snprintf(lines[2], sizeof(lines[2]), "Decode %.0f ms | Wait %.1f ms", g_decode_lat_us / 1000.0,
                 g_wait_lat_us / 1000.0);
        snprintf(lines[3], sizeof(lines[3]), "Drops %u | Repeats %u | Late %u | Lost %u", drops, repeats, late, lost);
        const char *text[4] = {lines[0], lines[1], lines[2], lines[3]};
        const uint32_t warn = 0xffa030;
        const uint32_t rgb[4] = {0xffffff, 0xffffff, 0xffffff, drops || late || lost ? warn : 0xffffff};
        EyeFov fov[2];
        headset_fov(fov);
        const float tangents[2][4] = {{fov[0].tan_left, fov[0].tan_right, fov[0].tan_up, fov[0].tan_down},
                                      {fov[1].tan_left, fov[1].tan_right, fov[1].tan_up, fov[1].tan_down}};
        video_set_overlay(text, rgb, 4, tangents);
        g_hud_shown = true;
    }
    start = now;
    frames = repeats = 0;
    last = ps;
    last_bytes = vs.bytes_total;
    last_dropped = vs.dropped;
    last_lost = vs.lost;
}

static uint64_t g_prev_display_us; // display time of the last new frame (ALVR frame interval)

// Compositor pass timing (paced stream). The compositor reads the last submitted frame right
// after it triggers its pass event (reference/decomp/hmd_thread.c: the trigger, then the
// submitted frame index is read), so a frame submitted when the loop was woken by the event
// only made the next pass, 11 ms later. The loop now sleeps after the event and submits
// shortly before the next pass (pass_wait_and_lead).
static uint64_t g_pass_period_us = 11111; // measured between pass events
static uint64_t g_next_pass_us;           // expected next pass event (0: unknown)
static uint64_t g_submit_lead_us = 2500;  // wake-up this long before it (grows after a miss)
static uint64_t g_loop_work_us;           // loop work from the wake-up to the submission (decaying peak)
static uint64_t g_loop_resume_us;         // wake-up time of this loop iteration
static unsigned g_pass_misses;            // submissions that came after the pass event
static const uint64_t LEAD_MAX_US = 6000;
// From the pass event to the photons (reprojection render, flip at vblank, panel): an
// estimate, for the latency reported.
static const uint64_t LATCH_TO_PHOTON_US = 4000;

// No stream frame shown (lobby, paused, disconnected): no prediction, and the latency, the
// frame interval and the overlay start again from the next frame shown.
static void video_not_shown()
{
    g_tracker_controller_prediction_us = 0; // lobby: show the Moves where they are
    g_tracker_head_prediction_us = 0;
    g_m2p_avg_us = 0;
    g_prev_display_us = 0;
    g_next_pass_us = 0; // the lobby does not follow the passes: the first wait is not a miss
    hud_clear();
}

// Waits for the compositor's pass event, then sleeps until shortly before the next one, so
// that the loop's work and the submission end just before that pass reads the frame. A
// submission that comes after the pass (the event was already there when waited for) is a
// miss: the lead grows by 0.5 ms; it shrinks back slowly towards the work time + 1 ms.
static bool pass_wait_and_lead()
{
    const uint64_t t0 = sceKernelGetProcessTime();
    if (!reproj_wait_frame(25000)) {
        g_next_pass_us = 0;
        return false;
    }
    const uint64_t e = sceKernelGetProcessTime();
    static uint64_t last_e;
    const uint64_t floor_us = g_loop_work_us + 1000 < LEAD_MAX_US ? g_loop_work_us + 1000 : LEAD_MAX_US;
    if (e - t0 < 200 && g_next_pass_us) {
        g_pass_misses++;
        g_submit_lead_us = g_submit_lead_us + 500 > LEAD_MAX_US ? LEAD_MAX_US : g_submit_lead_us + 500;
    } else if (g_submit_lead_us > floor_us + 10) {
        g_submit_lead_us -= 10;
    }
    if (g_submit_lead_us < floor_us)
        g_submit_lead_us = floor_us;
    if (last_e && e > last_e && e - last_e < 2 * g_pass_period_us)
        g_pass_period_us = (g_pass_period_us * 63 + (e - last_e)) / 64;
    last_e = e;
    g_next_pass_us = e + g_pass_period_us;
    const uint64_t target = g_next_pass_us > g_submit_lead_us ? g_next_pass_us - g_submit_lead_us : e;
    uint64_t now = sceKernelGetProcessTime();
    if (target > now + 400)
        sceKernelUsleep((uint32_t)(target - now - 300));
    while ((now = sceKernelGetProcessTime()) < target) {
    }
    g_loop_resume_us = now;
    return true;
}

// Streamed frame: shown through the compositor with the pose it was rendered for.
// Returns false (lobby shown instead) until frames arrive, or after 1.5 s without one.
static bool render_video()
{
    // Paced: one decision per stream frame period (every refresh at 90 Hz, every other at
    // 120 Hz), just before a compositor pass: the newest frame due is shown (video.cpp).
    // Shown as soon as converted instead, frames reached the compositor at irregular points
    // of its cycle (network, decoding and conversion times vary by a few ms, and the PC
    // runs at 90.00 fps against the headset's 89.91 Hz): at 90 Hz one frame in the 11.1 ms
    // window often came a pass late and the next one replaced it, a constant judder in the
    // headset (worse on a base PS4); one frame is skipped every ~11 s for the rate difference.
    static unsigned tick;
    const unsigned per_frame = g_display_hz == 90 ? 1 : 2;
    const bool take = !g_paced || ++tick % per_frame == 0;
    VideoFrame vf;
    if (!video_next(&vf, take, g_paced) || sceKernelGetProcessTime() - vf.decoded_us > 1500000) {
        video_not_shown();
        return false;
    }
    const uint64_t taken_us = sceKernelGetProcessTime();
    // Frames out of line (hardware report: now and then, mostly while turning, the headset
    // shows for one frame what looks like the picture of 3-4 frames before): a pose not
    // found, a tracking timestamp older than the previous frame's, or much older than usual.
    static unsigned pose_misses, backwards, stale, anomaly_logs;
    static uint64_t prev_ts;
    const bool fresh_frame = vf.seq != g_video_shown_seq;
    ReprojPose pose;
    const bool pose_found = find_sent_pose(vf.timestamp_ns, &pose);
    if (fresh_frame) {
        const uint64_t now_a = sceKernelGetProcessTime(), sample_us = vf.timestamp_ns / 1000;
        const int64_t step_ms = ((int64_t)vf.timestamp_ns - (int64_t)prev_ts) / 1000000;
        const int64_t age_ms = sample_us ? ((int64_t)now_a - (int64_t)sample_us) / 1000 : -1;
        const char *what = !pose_found                                      ? "pose not found"
                           : prev_ts && vf.timestamp_ns < prev_ts            ? "tracking timestamp went back"
                           : g_m2p_avg_us && age_ms * 1000 > (int64_t)g_m2p_avg_us + 30000 ? "old tracking timestamp"
                                                                             : nullptr;
        if (!pose_found)
            pose_misses++;
        else if (prev_ts && vf.timestamp_ns < prev_ts)
            backwards++;
        else if (what)
            stale++;
        if (what && anomaly_logs < 60) {
            anomaly_logs++;
            LOG("video: frame %u %s: %lld ms after the previous frame's, %lld ms old (m2p %.0f ms), %d waiting",
                vf.seq, what, (long long)step_ms, (long long)age_ms, g_m2p_avg_us / 1000.0, vf.waiting);
        }
        prev_ts = vf.timestamp_ns;
    }
    if (!pose_found) {
        // Unknown timestamp (0 = the streamer found no match): current pose, no warp.
        const TrackerPose &tp = g_tracker.device_pose;
        memset(&pose, 0, sizeof(pose));
        pose.timestamp = g_tracker.timestamp;
        pose.orientation[0] = tp.qx;
        pose.orientation[1] = tp.qy;
        pose.orientation[2] = tp.qz;
        pose.orientation[3] = tp.qw;
        pose.position[0] = tp.px;
        pose.position[1] = tp.py;
        pose.position[2] = tp.pz;
    }
    EyeFov fov[2];
    headset_fov(fov);
    float fov_block[2][4];
    compute_fov_blocks(fov, fov_block);
    bool ok = reproj_submit_stereo(vf.eye[0], vf.eye[1], fov_block[0], fov_block[1], &pose) == 0;
    static uint64_t stat_start;
    static unsigned shown;
    unsigned &last_seq = g_video_shown_seq;
    const bool fresh = vf.seq != last_seq;
    // Motion-to-photon: from the tracking sample a frame was rendered with to its display
    // (next compositor vsync, about one refresh period after submission). The headset pose
    // sent to SteamVR is predicted over it. The controllers are not: SteamVR extrapolates
    // them itself with their velocities, and predicting all of it on top was too much; the
    // optional extra prediction comes from the settings.
    uint64_t &m2p_avg_us = g_m2p_avg_us;
    uint64_t now = sceKernelGetProcessTime();
    g_tracker_controller_prediction_us = (uint32_t)g_config.controller_prediction_ms * 1000;
    // The headset position is predicted over the share of it set in the settings: 40 % by
    // default (hardware test: 100 % overshot, the view went past the head and came back).
    const uint64_t latency_us = m2p_avg_us < HEAD_PREDICTION_LATENCY_MAX_US ? m2p_avg_us : HEAD_PREDICTION_LATENCY_MAX_US;
    g_tracker_head_prediction_us = (uint32_t)(latency_us * (uint64_t)g_config.head_prediction_percent / 100);
    if (vf.seq != last_seq) {
        shown++;
        last_seq = vf.seq;
        uint64_t sample_us = vf.timestamp_ns / 1000;
        const uint64_t vsync = 1000000 / g_display_hz;
        // Shown by the next pass (paced: submitted just before it), else by the one after.
        const uint64_t display_us = g_paced && g_next_pass_us > now && g_next_pass_us - now < 2 * vsync
                                        ? g_next_pass_us + LATCH_TO_PHOTON_US
                                        : now + vsync + LATCH_TO_PHOTON_US;
        if (sample_us && display_us > sample_us && display_us - sample_us < 500000) {
            uint64_t m2p = display_us - sample_us;
            m2p_avg_us = m2p_avg_us ? (m2p_avg_us * 31 + m2p) / 32 : m2p;
        }
        if (vf.received_us && vf.picture_us >= vf.received_us && now >= vf.picture_us) {
            const uint64_t dec = vf.picture_us - vf.received_us, wait = now - vf.picture_us;
            g_decode_lat_us = g_decode_lat_us ? (g_decode_lat_us * 15 + dec) / 16 : dec;
            g_wait_lat_us = g_wait_lat_us ? (g_wait_lat_us * 15 + wait) / 16 : wait;
        }
        // ALVR dashboard statistics for this frame (its stages in process time, each one
        // starting where the previous ended).
        uint64_t &prev_display_us = g_prev_display_us;
        if (vf.timestamp_ns && sample_us < display_us && vf.received_us && vf.picture_us >= vf.received_us &&
            taken_us >= vf.picture_us) {
            AlvrFrameStatistics fs;
            fs.target_timestamp_ns = vf.timestamp_ns;
            fs.frame_interval_ns = prev_display_us && display_us > prev_display_us ? (display_us - prev_display_us) * 1000 : 0;
            fs.video_decode_ns = (vf.picture_us - vf.received_us) * 1000;
            fs.video_decoder_queue_ns = (taken_us - vf.picture_us) * 1000;
            fs.rendering_ns = (now - taken_us) * 1000;
            fs.vsync_queue_ns = (display_us - now) * 1000;
            fs.total_pipeline_latency_ns = (display_us - sample_us) * 1000;
            alvr_send_statistics(&fs);
        }
        prev_display_us = display_us;
    }
    update_hud(now, fresh, take && !fresh);
    // Display frames without a new frame to show although the stream runs (the reserve was
    // empty), and the FIFO level seen.
    static unsigned repeats, takes, waiting_sum;
    if (take && !fresh)
        repeats++;
    if (take) {
        takes++;
        waiting_sum += vf.waiting;
    }
    // How far the prediction moved the headset (largest over the report period).
    static float lead_max_cm;
    if (g_tracker.head_lead_m * 100.0f > lead_max_cm)
        lead_max_cm = g_tracker.head_lead_m * 100.0f;
    if (!stat_start)
        stat_start = now;
    if (now - stat_start >= 5000000) {
        VideoStats vs;
        video_get_stats(&vs);
        static VideoPacingStats last;
        VideoPacingStats ps;
        video_pacing_stats(&ps);
        // skip: frames never shown (display FIFO full, surplus skipped, conversion behind);
        // late: frames that came after their display frame; arrival: gaps in the frames
        // received (above 1.8x the usual interval), frames bunched (< 2 ms apart), longest gap.
        LOG("video: %.1f fps, rx %u dec %u drop %u lost %u err %u, decode %.1f ms (cpu %.1f), %.0f KB/frame, convert %.1f ms, "
            "queue %u, m2p %.0f ms, head predicted %.0f ms (up to %.1f cm), pacing %s: repeat %u skip %u late %u "
            "margin %.1f ms hold %.1f ms resync %u decode %.1f ms wait %.1f ms lead %.1f ms misses %u, arrival gaps %u bunched %u max %.0f ms, "
            "pose miss %u back %u old %u",
            shown * 1e6 / (double)(now - stat_start), vs.received, vs.decoded, vs.dropped, vs.lost, vs.errors,
            vs.decode_us_avg / 1000.0, vs.decode_cpu_us_avg / 1000.0, vs.bytes_avg / 1024.0, vs.convert_us_avg / 1000.0,
            vs.queue_max, m2p_avg_us / 1000.0, g_tracker_head_prediction_us / 1000.0, lead_max_cm,
            g_paced ? "on" : "off", repeats,
            (ps.overflow - last.overflow) + (ps.trimmed - last.trimmed) + (ps.replaced - last.replaced),
            ps.late - last.late, ps.margin_us / 1000.0, ps.hold_us / 1000.0, ps.resyncs - last.resyncs, g_decode_lat_us / 1000.0, g_wait_lat_us / 1000.0,
            g_submit_lead_us / 1000.0, g_pass_misses, vs.arrival_gaps,
            vs.arrival_bunched, vs.arrival_gap_max_us / 1000.0, pose_misses, backwards, stale);
        pose_misses = backwards = stale = 0;
        stat_start = now;
        shown = 0;
        lead_max_cm = 0;
        repeats = takes = waiting_sum = 0;
        g_pass_misses = 0;
        last = ps;
    }
    return ok;
}

// Lobby model of a PS Move (always shown while tracked; the 10 s "searching" state is only
// for SteamVR). The emulated trackpad is filled by the caller.
static void fill_lobby_move(LobbyView::Controller *c, const MoveController &m, char letter)
{
    c->visible = m.connected && m.track.registered && m.track.has_position;
    c->pos = v3(m.track.position[0], m.track.position[1], m.track.position[2]);
    c->rot = Quat{m.track.orientation[0], m.track.orientation[1], m.track.orientation[2], m.track.orientation[3]};
    c->rgb = move_led_rgb(m.track.led_color);
    c->tracked = m.track.position_quality == 9 || m.track.position_quality == 6;
    c->hand_letter = letter;
    c->buttons = m.connected ? m.buttons : 0;
    c->trigger = m.connected ? m.trigger / 255.0f : 0.0f;
    c->battery = -1.0f;
    move_battery(m, &c->battery, &c->charging);
}

// A DualShock 4 that is connected but not tracked (the tracker runs two controllers, the
// PS Moves first) is shown still, in grey, in front of the play area, so its buttons can
// still be seen; `slot` spreads several of them.
static void fill_lobby_pad(LobbyView::Pad *d, const PadController &p, char label, int slot)
{
    const TrackedDevice &tr = p.track;
    const bool tracked = tr.registered && tr.has_position;
    d->visible = p.connected;
    d->floating = !tracked;
    if (tracked) {
        d->pos = v3(tr.position[0], tr.position[1], tr.position[2]);
        d->rot = Quat{tr.orientation[0], tr.orientation[1], tr.orientation[2], tr.orientation[3]};
    } else {
        const float tilt = 0.70f; // top face turned towards the user (radians)
        d->pos = v3(g_center_x + (slot - 0.5f * (LOBBY_PADS - 1)) * 0.25f, g_floor_y + 1.0f, g_center_z - 0.55f);
        d->rot = Quat{sinf(tilt / 2), 0, 0, cosf(tilt / 2)};
    }
    d->rgb = move_led_rgb(tr.led_color);
    d->tracked = tracked && (tr.position_quality == 9 || tr.position_quality == 6);
    d->buttons = p.buttons;
    d->lx = p.lx;
    d->ly = p.ly;
    d->rx = p.rx;
    d->ry = p.ry;
    d->l2 = p.l2;
    d->r2 = p.r2;
    for (int k = 0; k < 2; k++) {
        d->touch[k] = p.touch[k].down;
        d->touch_x[k] = p.touch[k].x;
        d->touch_y[k] = p.touch[k].y;
    }
    d->battery = -1.0f;
    pad_battery(p, &d->battery, &d->charging);
    d->rumble_large = p.vib_large / 255.0f;
    d->rumble_small = p.vib_small / 255.0f;
    d->label = label;
}

// Stereo lobby, rendered in software into the side-by-side buffer and handed to the
// system compositor together with the pose it was rendered for.
static bool render_lobby(Screen *s)
{
    (void)s;
    const float floor_y = g_floor_y;
    const TrackerPose &tp = g_tracker.device_pose;
    const uint64_t now = sceKernelGetProcessTime();
    LobbyView view;
    memset(&view, 0, sizeof(view));
    // Render each eye from the tracker's own eye pose: the device pose is not at eye
    // level and does not rotate about the neck, which put the camera too low and made
    // the perspective wrong when turning the head.
    for (int eye = 0; eye < 2; eye++) {
        const TrackerPose &ep = g_tracker.eye_pose[eye];
        float n = ep.qx * ep.qx + ep.qy * ep.qy + ep.qz * ep.qz + ep.qw * ep.qw;
        if (n > 0.5f) {
            view.eye_pos[eye] = v3(ep.px, ep.py, ep.pz);
            view.eye_rot[eye] = Quat{ep.qx, ep.qy, ep.qz, ep.qw};
        } else { // eye poses not filled: device pose +- half a default IPD
            Quat q{tp.qx, tp.qy, tp.qz, tp.qw};
            view.eye_pos[eye] = v3(tp.px, tp.py, tp.pz) + rotate(q, v3(eye ? 0.0315f : -0.0315f, 0, 0));
            view.eye_rot[eye] = q;
        }
    }
    static bool logged = false;
    if (!logged && g_tracker.status == 1) {
        logged = true;
        const TrackerPose &l = g_tracker.eye_pose[0], &r = g_tracker.eye_pose[1], &h = g_tracker.head_pose;
        float dx = r.px - l.px, dy = r.py - l.py, dz = r.pz - l.pz;
        LOG("poses: device p=(%.3f %.3f %.3f) left eye p=(%.3f %.3f %.3f) right eye p=(%.3f %.3f %.3f) head p=(%.3f %.3f %.3f) ipd=%.4f",
            tp.px, tp.py, tp.pz, l.px, l.py, l.pz, r.px, r.py, r.pz, h.px, h.py, h.pz, sqrtf(dx * dx + dy * dy + dz * dz));
    }
    float hp[3], hq[4];
    head_pose(hp, hq);
    view.head_pos = v3(hp[0], hp[1], hp[2]);
    view.head_rot = Quat{hq[0], hq[1], hq[2], hq[3]};
    const HmdFieldOfView &f = g_hmd.fov;
    view.fov[0] = EyeFov{f.tan_out, f.tan_in, f.tan_top, f.tan_bottom};
    view.fov[1] = EyeFov{f.tan_in, f.tan_out, f.tan_top, f.tan_bottom};
    view.floor_y = floor_y;
    view.center_x = g_center_x;
    view.center_z = g_center_z;
    SettingsRay rays[LOBBY_POINTERS];
    memset(rays, 0, sizeof(rays));
    for (int i = 0; i < MOVE_MAX; i++) {
        LobbyView::Controller &c = view.controllers[i];
        const WandInput &w = g_wand[i];
        fill_lobby_move(&c, g_moves[i], move_index_hand(i) == HAND_LEFT ? 'L' : 'R');
        c.pad_touch = w.pad_touch;
        c.pad_click = w.pad_click;
        c.pad_x = w.pad_x;
        c.pad_y = w.pad_y;
        // Settings laser: from the sphere along the controller's forward (-Z).
        rays[i].valid = c.visible && g_moves[i].track.has_orientation;
        rays[i].origin = c.pos;
        rays[i].dir = rotate(c.rot, v3(0, 0, -1));
        rays[i].trigger = c.trigger;
        rays[i].seen = c.tracked;
    }
    // DualShock 4: laser from the light bar along the pad's forward (-Z), Cross clicks.
    {
        LobbyView::Pad &d = view.pads[0];
        fill_lobby_pad(&d, g_pad, 0, 0);
        SettingsRay &r = rays[MOVE_MAX];
        r.valid = d.visible && !d.floating && g_pad.track.has_orientation;
        r.origin = d.pos;
        r.dir = rotate(d.rot, v3(0, 0, -1));
        r.trigger = (g_pad.buttons & PAD_BUTTON_CROSS) ? 1.0f : 0.0f;
        r.seen = d.tracked;
        r.scroll = g_pad.connected ? g_pad.ry : 0.0f; // right stick scrolls the settings
    }
    // Other users' controllers, labelled with the user number (no lasers).
    for (int k = 0; k < g_other_count && k < OTHER_USERS; k++) {
        const char label = (char)('2' + k);
        for (int i = 0; i < MOVE_MAX; i++)
            fill_lobby_move(&view.controllers[MOVE_MAX * (k + 1) + i], g_others[k].moves[i], label);
        fill_lobby_pad(&view.pads[k + 1], g_others[k].pad, label, k + 1);
    }
    view.time_s = now / 1e6f;

    // Settings panel (START in the lobby), or the first launch wizard.
    static LobbyPanel panel;
    if (g_wizard_pending && !settings_is_open() && !headset_lost(now))
        settings_open_wizard(v3(hp[0], hp[1], hp[2]));
    if (settings_is_open()) {
        // Controllers that can point: connected and tracked (a Move the tracker refused cannot).
        int pointers_on = g_pad.connected && g_pad.track.registered;
        for (int i = 0; i < MOVE_MAX; i++)
            pointers_on += g_moves[i].connected && g_moves[i].track.registered;
        SettingsContext ctx{&g_config, floor_y, hp[1], pointers_on, v3(hp[0], hp[1], hp[2])};
        int clicked = -1;
        unsigned actions = settings_update(ctx, rays, now, &clicked);
        if (clicked >= 0 && clicked < MOVE_MAX && lobby_haptics_allowed())
            move_vibrate(&g_moves[clicked], 150, 30);
        else if (clicked == MOVE_MAX && lobby_haptics_allowed())
            pad_vibrate(&g_pad, 0, 160, 40);
        if ((actions & SETTINGS_VIBRATION_SET) && lobby_haptics_allowed()) { // a sample at the new strength
            if (clicked >= 0 && clicked < MOVE_MAX)
                move_vibrate(&g_moves[clicked], 255, 250);
            else if (clicked == MOVE_MAX)
                pad_vibrate(&g_pad, 255, 255, 250);
        }
        if ((actions & SETTINGS_CALIBRATED) && lobby_haptics_allowed()) { // height measured: a longer buzz
            for (int i = 0; i < MOVE_MAX; i++)
                move_vibrate(&g_moves[i], 200, 250);
            pad_vibrate(&g_pad, 0, 200, 250);
        }
        if (actions & SETTINGS_HEIGHT_CHANGED)
            g_floor_y = -g_config.camera_height_cm / 100.0f;
        if (actions & SETTINGS_RESET) {
            g_floor_set = false; // guessed again from the headset height
            g_wizard_pending = true; // the height wizard opens on the next frame
        }
        if ((actions & SETTINGS_CONFIRMED) && g_wizard_pending) {
            g_wizard_pending = false;
            start_alvr(); // once: after a Reset the PC link is already running
        }
    }
    settings_build(&panel, view.pointers);
    view.panel = &panel;
    view.floor_y = g_floor_y;

    // Headset lost by the camera for 3 s: the surroundings (grid, info, settings) fade to
    // black in 0.5 s; the PS Camera and the controllers stay where they are. Headset
    // tracking not started yet (the headset pose is then at the camera): only the camera
    // is shown, 1.8 m ahead, blinking blue and red every 0.8 s.
    static float lost_black = 1.0f;
    static uint64_t last_us = now;
    const float step = (now - last_us) / 500000.0f;
    last_us = now;
    if (headset_lost(now))
        lost_black = lost_black + step > 1.0f ? 1.0f : lost_black + step;
    else
        lost_black = lost_black - step < 0.0f ? 0.0f : lost_black - step;
    view.brightness = 1.0f - lost_black;
    view.overlay_text = nullptr;
    if (!g_hmd_tracking_init) {
        view.beacon = true;
        view.beacon_pos = v3(hp[0], hp[1], hp[2] - 1.8f);
        view.beacon_rgb = (now / 800000) % 2 ? 0xff4040 : 0x40c0ff;
    }
    // First launch: no grid while the floor is only a guess (until the height is set).
    view.grid_visible = !(g_wizard_pending && !settings_wizard_height_set());

    // Info panel, once, far in front (towards the camera, 3 m beyond it).
    static char info_lines[7][96];
    snprintf(info_lines[0], sizeof(info_lines[0]), "%s", ALVR_PS4_TITLE);
    AlvrStatus st;
    alvr_get_status(&st);
    const bool waiting_height = g_wizard_pending && !g_alvr_started;
    switch (waiting_height ? ALVR_DISCOVERY : st.state) {
    case ALVR_DISCOVERY:
        if (waiting_height) {
            snprintf(info_lines[1], sizeof(info_lines[1]), "Set your height to continue");
            break;
        }
        snprintf(info_lines[1], sizeof(info_lines[1]), "Waiting for the PC (ALVR streamer %s)", ALVR_STREAMER_VERSION);
        break;
    case ALVR_WAITING:
        snprintf(info_lines[1], sizeof(info_lines[1]), "PC found (%s), waiting for SteamVR...", st.server_ip);
        break;
    case ALVR_HANDSHAKE:
        snprintf(info_lines[1], sizeof(info_lines[1]), "Connecting to %s...", st.server_ip);
        break;
    case ALVR_RESTARTING:
        snprintf(info_lines[1], sizeof(info_lines[1]), "SteamVR is restarting on %s...", st.server_ip);
        break;
    case ALVR_STREAMING:
        if (g_stream_paused)
            snprintf(info_lines[1], sizeof(info_lines[1]), "Connected to %s - the game is paused in the lobby",
                     st.server_ip);
        else
            snprintf(info_lines[1], sizeof(info_lines[1]), "Connected to %s (%ux%u, %.0f Hz) - %u video packets",
                     st.server_ip, st.view_width, st.view_height, st.refresh_rate, st.video_packets);
        break;
    }
    snprintf(info_lines[2], sizeof(info_lines[2]), "Hostname: %s", g_config.hostname);
    snprintf(info_lines[3], sizeof(info_lines[3]), "IP: %s", g_ip);
    snprintf(info_lines[4], sizeof(info_lines[4]), "Client v%s", ALVR_PS4_VERSION);
    // How to open the settings, with the controllers that are connected.
    bool any_move = false;
    for (int i = 0; i < MOVE_MAX; i++)
        any_move = any_move || g_moves[i].connected;
    const char *open_hint = any_move && g_pad.connected ? "Press Start (PS Move) or Options (DualShock 4) to open settings"
                            : any_move                 ? "Press Start on a PS Move to open settings"
                            : g_pad.connected          ? "Press Options on the DualShock 4 to open settings"
                                                       : "Connect a PS Move or a DualShock 4 to open settings";
    snprintf(info_lines[5], sizeof(info_lines[5]), "%s", g_wizard_pending ? "" : open_hint);
    int info_count = 6;
    if (st.state == ALVR_STREAMING && !waiting_height) {
        snprintf(info_lines[6], sizeof(info_lines[6]), "%s",
                 g_stream_paused ? "Hold Cross (left Move) or Circle (right Move) to go back to the game"
                                 : "Hold Cross (left Move) or Circle (right Move) for the lobby");
        info_count = 7;
    }
    for (int i = 0; i < info_count; i++)
        view.info[i] = info_lines[i];
    view.info[info_count] = nullptr;
    if (g_wizard_pending) // nothing behind the first launch wizard
        view.info[0] = nullptr;
    view.info_pos = v3(0.0f, g_floor_y + 1.6f, -3.0f);
    view.info_yaw = 0.0f;
    // Each eye gets its own 960x1080 image with pitch == width: with both eyes in one
    // 1920-wide buffer the compositor ignored the pitch and mixed the eyes row by row.
    static uint32_t *eye_buf[2][2]; // [double-buffer index][eye]
    static bool alloc_failed;       // not retried at every frame
    const int eye_w = 960, eye_h = 1080;
    if (!eye_buf[0][0]) {
        if (alloc_failed)
            return false;
        const size_t each = (size_t)eye_w * eye_h * 4, align = 0x10000;
        const size_t total = (each * 4 + align - 1) / align * align;
        off_t phys = 0;
        void *mem = nullptr;
        // WB onion (type 0), CPU-cached: the renderer reads pixels back (AA blending),
        // which is extremely slow on write-combined garlic memory. The GPU reads onion too.
        alloc_failed = true;
        if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), total, align, 0, &phys) < 0) {
            LOG("lobby: eye buffer allocation failed");
            return false;
        }
        if (sceKernelMapDirectMemory(&mem, total, 0x33, 0, phys, align) < 0) {
            LOG("lobby: eye buffer mapping failed");
            sceKernelReleaseDirectMemory(phys, total);
            return false;
        }
        alloc_failed = false;
        for (int i = 0; i < 4; i++)
            eye_buf[i / 2][i % 2] = (uint32_t *)((char *)mem + each * i);
    }
    static int cur = 0;
    static GnmTexture eye_tex[2][2];
    uint64_t t0 = sceKernelGetProcessTime();
    for (int eye = 0; eye < 2; eye++) {
        lobby_render_eye(eye_buf[cur][eye], eye_w, eye_h, eye_w, &view, eye);
        gnm_texture_linear_bgra(&eye_tex[cur][eye], eye_buf[cur][eye], eye_w, eye_h, eye_w);
    }
    float fov_block[2][4];
    compute_fov_blocks(view.fov, fov_block);
    ReprojPose pose;
    memset(&pose, 0, sizeof(pose));
    pose.timestamp = g_tracker.timestamp;
    pose.orientation[0] = tp.qx;
    pose.orientation[1] = tp.qy;
    pose.orientation[2] = tp.qz;
    pose.orientation[3] = tp.qw;
    pose.position[0] = tp.px;
    pose.position[1] = tp.py;
    pose.position[2] = tp.pz;
    uint64_t t1 = sceKernelGetProcessTime();
    bool ok = reproj_submit_stereo(&eye_tex[cur][0], &eye_tex[cur][1], fov_block[0], fov_block[1], &pose) == 0;
    // Timing report every 5 s: render cost and effective frame rate.
    static uint64_t stat_start = 0, render_sum = 0, render_max = 0;
    static unsigned stat_frames = 0;
    if (!stat_start)
        stat_start = t0;
    render_sum += t1 - t0;
    if (t1 - t0 > render_max)
        render_max = t1 - t0;
    stat_frames++;
    if (t1 - stat_start >= 5000000) {
        LOG("lobby: %.1f fps, render avg %.2f ms max %.2f ms", stat_frames * 1e6 / (double)(t1 - stat_start),
            render_sum / 1000.0 / stat_frames, render_max / 1000.0);
        stat_start = t1;
        render_sum = render_max = 0;
        stat_frames = 0;
    }
    cur ^= 1;
    return ok;
}

// ---- ALVR uplink -------------------------------------------------------------------

static void haptics_to_move(int hand, float duration_s, float frequency, float amplitude)
{
    (void)frequency;
    if (g_stream_paused) // in the lobby: the game's vibrations are ignored
        return;
    // ALVR hand 0 = left, 1 = right; Move index 0 is the right hand.
    int index = hand == 1 ? 0 : 1;
    float a = amplitude < 0 ? 0 : amplitude > 1 ? 1 : amplitude;
    uint32_t ms = (uint32_t)(duration_s * 1000.0f);
    move_vibrate(&g_moves[index], (uint8_t)(a * 255.0f), ms < 10 ? 10 : ms);
}

static void start_alvr()
{
    if (g_alvr_started)
        return;
    g_alvr_started = true;
    AlvrViews views;
    views.view_width = (uint32_t)(960 * g_config.resolution_percent / 100);
    views.view_height = (uint32_t)(1080 * g_config.resolution_percent / 100);
    views.ipd_m = 0.063f;
    views.fps = (float)stream_fps();
    const HmdFieldOfView &f = g_hmd.fov;
    // OpenXR angle convention: left and down negative.
    const float l[2] = {f.tan_out, f.tan_in}, r[2] = {f.tan_in, f.tan_out};
    for (int e = 0; e < 2; e++) {
        views.fov[e][0] = -atanf(l[e]);
        views.fov[e][1] = atanf(r[e]);
        views.fov[e][2] = atanf(f.tan_top);
        views.fov[e][3] = -atanf(f.tan_bottom);
    }
    alvr_start(g_config.hostname, g_ip, &views, haptics_to_move);
}

// Tracker space (origin at the PS Camera, +Y up, user looking at -Z) to ALVR stage space
// (same axes, origin on the floor).
static void to_stage(const float p[3], const float q[4], AlvrDeviceMotion *m)
{
    m->present = true;
    m->position[0] = p[0] - g_center_x;
    m->position[1] = p[1] - g_floor_y;
    m->position[2] = p[2] - g_center_z;
    for (int i = 0; i < 4; i++)
        m->orientation[i] = q[i];
    memset(m->linear_velocity, 0, sizeof(m->linear_velocity));
    memset(m->angular_velocity, 0, sizeof(m->angular_velocity));
}

static void send_alvr_uplink()
{
    AlvrDeviceMotion head, hands[2];
    // Head: centre between the eyes, orientation of the headset (always sent), its position
    // predicted to the time the frame rendered with it is shown, as the official client
    // does: SteamVR does not extrapolate the headset (ALVR sends it no velocity), so the
    // current position made the view lag the head by the whole stream latency (40-70 ms;
    // "jelly" position; the PS4 reprojection corrects only the rotation).
    float hp[3], hq[4];
    head_pose(hp, hq, true);
    to_stage(hp, hq, &head);
    const uint64_t now = sceKernelGetProcessTime();
    // Headset tracking not initialized yet (start, back from the PS menu): searching right
    // away. Once initialized: searching after 3 s without the camera seeing it (grey
    // screen). The 20.14.1 protocol cannot say so; the patched driver
    // (tools/alvr_driver_patch.py) treats a head below -500 m as out of range.
    bool head_lost = headset_lost(now);
    static bool head_lost_logged;
    if (head_lost != head_lost_logged) {
        LOG("headset %s", !head_lost             ? "tracked again"
                          : !g_hmd_tracking_init ? "tracking not initialized: reported as searching to SteamVR"
                                                 : "lost for 3 s: reported as searching to SteamVR");
        head_lost_logged = head_lost;
    }
    if (head_lost)
        head.position[1] = -1000.0f;
    for (int i = 0; i < MOVE_MAX; i++) {
        const MoveController &m = g_moves[i];
        int hand = move_index_hand(i) == HAND_LEFT ? 0 : 1;
        AlvrDeviceMotion &hm = hands[hand];
        to_stage(m.track.position, m.track.orientation, &hm);
        // Tracker-space velocities (stage space only shifts y): SteamVR extrapolates the
        // controllers with them over its own pipeline latency.
        memcpy(hm.linear_velocity, m.track.velocity, sizeof(hm.linear_velocity));
        memcpy(hm.angular_velocity, m.track.angular_velocity, sizeof(hm.angular_velocity));
        // A controller switched on is always sent (omitted = disconnected, inputs dropped).
        // Lost by the camera it keeps its last pose for 10 s, then (or when never seen) it
        // is marked searching with a height below -500 m: the patched driver
        // (tools/alvr_driver_patch.py) hides it but keeps its buttons working.
        hm.present = m.connected;
        if (!m.track.has_position || !m.track.last_seen_us ||
            now - m.track.last_seen_us > TRACKER_CONTROLLER_SEARCHING_US)
            hm.position[1] = -1000.0f;
        float gauge = 0.0f;
        bool charging = false;
        const bool battery_known = move_battery(m, &gauge, &charging);
        alvr_set_battery(hand, battery_known, gauge, charging);
        const WandInput &w = g_wand[i];
        AlvrHandInput in;
        in.trackpad_touch = w.pad_touch;
        in.trackpad_click = w.pad_click;
        in.trackpad_x = w.pad_x;
        in.trackpad_y = w.pad_y;
        in.grip = w.grip;
        in.menu = w.menu;
        in.system = w.system;
        in.trigger = w.trigger;
        in.trigger_click = w.trigger_click;
        if (g_stream_paused) // in the lobby: SteamVR sees every button released
            memset(&in, 0, sizeof(in));
        alvr_update_input(hand, &in);
    }
    remember_sent_pose(now * 1000ull);
    alvr_send_tracking(now * 1000ull, &head, &hands[0], &hands[1]);
}

// TV / floating 2D screen: title and one status line (no debug output).
static void draw_status(Screen *s, const char *status)
{
    screen_fill(s, 0x06080c);
    screen_text(s, 80, s->height / 2 - 40, ALVR_PS4_TITLE "  v" ALVR_PS4_VERSION, 0xffffff);
    screen_text(s, 80, s->height / 2, status, 0xa0c4ff);
}

// Before anything VR: wait until the headset is ready (connected, powered, processor
// unit USB plugged), showing the system's "connect your PlayStation VR" dialog meanwhile,
// reopened if canceled (as VR Worlds does). NOT_READY is also a headset powering on, so
// it gets 1 s before the dialog is asked for.
static void wait_for_headset(Screen *s)
{
    if (!hmd_init(g_probes[0].handle, &g_hmd)) {
        LOG("libSceHmd unavailable");
        return;
    }
    const bool dialog = hmd_setup_init(g_probes[10].handle, g_probes[11].handle);
    vr_service_dialog_init(g_probes[12].handle);
    uint32_t last_status = 0xffffffff;
    uint64_t status_since = 0, next_try_us = 0;
    for (;;) {
        hmd_refresh(&g_hmd);
        const uint32_t status = g_hmd.info.status;
        const uint64_t now = sceKernelGetProcessTime();
        if (status != last_status) {
            LOG("headset status: %s", hmd_status_name(status));
            last_status = status;
            status_since = now;
        }
        hmd_setup_poll();
        if (status == HMD_STATUS_READY && !hmd_setup_running())
            break;
        if (status != HMD_STATUS_READY && dialog && !hmd_setup_running() && now >= next_try_us &&
            (status != HMD_STATUS_NOT_READY || now - status_since > 1000000) && !hmd_setup_start(g_user_id))
            next_try_us = now + 1000000;
        if (s->handle > 0) {
            draw_status(s, "Connect PlayStation VR and turn it on");
            screen_flip(s);
        } else {
            sceKernelUsleep(16000);
        }
        sceGnmSubmitDone(); // the system's safe point to suspend or close the app
    }
    LOG("headset ready");
}

// While running: when the headset stops being ready (powered off, unplugged), the setup
// dialog asks for it again (not while the app is in the background). Once it is back,
// a handle made invalid by the power cycle is reopened and registered with the tracker
// again, and the tracking counts as not initialized until the camera sees the headset.
static void monitor_headset(unsigned frame, uint64_t now)
{
    static bool lost = false;
    static uint64_t status_since = 0, next_try_us = 0, reopen_retry_us = 0;
    if (!g_hmd.initialized)
        return;
    if (frame % 15 == 0) {
        const uint32_t prev = g_hmd.info.status;
        hmd_refresh(&g_hmd);
        if (g_hmd.info.status != prev) {
            LOG("HMD status changed: %s -> %s", hmd_status_name(prev), hmd_status_name(g_hmd.info.status));
            status_since = now;
        }
    }
    hmd_setup_poll();
    const uint32_t status = g_hmd.info.status;
    if (status != HMD_STATUS_READY) {
        if (!lost)
            LOG("headset not ready (%s)", hmd_status_name(status));
        lost = true;
        if (!g_in_background && !hmd_setup_running() && now >= next_try_us &&
            (status != HMD_STATUS_NOT_READY || now - status_since > 1000000) && !hmd_setup_start(g_user_id))
            next_try_us = now + 2000000;
    } else if (lost && !hmd_setup_running() && now >= reopen_retry_us) {
        if (!hmd_handle_valid(&g_hmd)) {
            // A reopen too early after the power cycle can fail: tried again every second.
            if (!hmd_reopen(g_user_id, &g_hmd)) {
                LOG("headset ready again, but it cannot be opened yet: retrying in 1 s");
                reopen_retry_us = now + 1000000;
                return;
            }
            tracker_reregister_hmd(g_hmd.handle);
        }
        lost = false;
        LOG("headset ready again");
        g_hmd_tracking_init = false;
        g_hmd_init_after_us = now + 300000;
    }
}

// Headset tracking not started for 10 s while the headset moves (it is worn, not lying
// somewhere the camera cannot see): the system's "confirm your position" screen is opened,
// once per run (VR service dialog, as VR Worlds does).
static void confirm_position_if_stuck(uint64_t now)
{
    static uint64_t since;
    static bool opened, have_ref, moved;
    static Quat ref;
    vr_service_dialog_poll();
    if (opened)
        return;
    if (g_hmd_tracking_init || g_in_background || hmd_setup_running() || !g_hmd.initialized) {
        since = 0;
        have_ref = moved = false;
        return;
    }
    if (!since)
        since = now;
    const TrackerPose &d = g_tracker.device_pose;
    const Quat q{d.qx, d.qy, d.qz, d.qw};
    if (q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w > 0.5f) {
        if (!have_ref) {
            ref = q;
            have_ref = true;
        } else if (!moved) {
            // Turned more than 10 degrees from where it was when the wait started.
            const float dot = fabsf(ref.x * q.x + ref.y * q.y + ref.z * q.z + ref.w * q.w);
            if (dot < cosf(0.5f * 10.0f * 0.0174533f)) {
                moved = true;
                LOG("headset moving while its tracking has not started");
            }
        }
    }
    if (moved && now - since >= 10000000 && vr_service_dialog_open())
        opened = true;
}

// Other users logged in on the console (checked every 3 s): their PS Moves and DualShock 4
// are opened and tracked like the playing user's, within the tracker's per-user limit.
static void poll_other_users(uint64_t now)
{
    static uint64_t next_us;
    if (now < next_us)
        return;
    next_us = now + 3000000;
    OrbisUserServiceLoginUserIdList list;
    memset(&list, 0xff, sizeof(list));
    if (sceUserServiceGetLoginUserIdList(&list) < 0)
        return;
    for (int i = 0; i < ORBIS_USER_SERVICE_MAX_LOGIN_USERS; i++) {
        const int id = list.userId[i];
        if (id == -1 || id == g_user_id || g_other_count >= OTHER_USERS)
            continue;
        bool known = false;
        for (int k = 0; k < g_other_count; k++)
            known = known || g_others[k].user == id;
        if (known)
            continue;
        OtherUser &o = g_others[g_other_count];
        o.user = id;
        LOG("other user 0x%x logged in: opening their controllers", id);
        move_start(g_probes[2].handle, id, o.moves);
        pad_start(id, &o.pad);
        g_other_count++;
    }
}

static void update_other_users()
{
    for (int k = 0; k < g_other_count; k++) {
        move_update(g_others[k].moves);
        pad_update(&g_others[k].pad);
    }
}

// DualShock 4 motors in the lobby, to try them: L2 drives the large motor (left grip), R2
// the small one (right grip). Click ticks (timed pulses) are left alone; once streaming,
// the pad stays still (nothing of it goes to SteamVR).
static void update_pad_rumble(bool pc_connected)
{
    const uint8_t large = pc_connected ? 0 : (uint8_t)(g_pad.l2 * 255.0f);
    const uint8_t small = pc_connected ? 0 : (uint8_t)(g_pad.r2 * 255.0f);
    if (large > 8 || small > 8)
        pad_vibrate(&g_pad, large, small, 0);
    else if (!g_pad.vibration_end_us && (g_pad.vib_large || g_pad.vib_small))
        pad_vibrate(&g_pad, 0, 0, 0);
}

// Lobby while streaming: holding the button each PS Move has no use for in the Vive wand
// mapping (Cross on the left Move, Circle on the right one; wand.cpp) for 1 s switches to
// the lobby, and again back to the stream (a fresh IDR frame is requested). Either Move's
// button alone does it.
static const uint64_t LOBBY_HOLD_US = 1000000;

static void handle_lobby_toggle(uint64_t now, bool pc_connected)
{
    static uint64_t held_since[MOVE_MAX];
    static bool fired; // toggled: again only once both buttons are released (both held = one toggle)
    // The first launch wizard (also after a settings Reset while connected) keeps the lobby:
    // the stream is paused until it is confirmed.
    static bool wizard_paused;
    if (pc_connected && g_wizard_pending && !g_stream_paused) {
        g_stream_paused = true;
        wizard_paused = true;
        video_set_paused(true);
        LOG("stream: paused for the first launch wizard");
    } else if (wizard_paused && !g_wizard_pending) {
        wizard_paused = false;
        if (g_stream_paused && pc_connected) {
            g_stream_paused = false;
            video_set_paused(false);
            LOG("stream: wizard confirmed, back to the game");
        }
    }
    if (!pc_connected) {
        wizard_paused = false;
        if (g_stream_paused) {
            g_stream_paused = false;
            video_set_paused(false);
            LOG("stream: PC disconnected while in the lobby");
        }
        memset(held_since, 0, sizeof(held_since));
        fired = false;
        return;
    }
    if (g_wizard_pending)
        return;
    bool any_held = false;
    for (int i = 0; i < MOVE_MAX; i++) {
        const uint16_t button = move_index_hand(i) == HAND_LEFT ? MOVE_BUTTON_CROSS : MOVE_BUTTON_CIRCLE;
        if (!g_moves[i].connected || !(g_moves[i].buttons & button)) {
            held_since[i] = 0;
            continue;
        }
        any_held = true;
        if (!held_since[i])
            held_since[i] = now;
        if (fired || now - held_since[i] < LOBBY_HOLD_US)
            continue;
        fired = true;
        g_stream_paused = !g_stream_paused;
        video_set_paused(g_stream_paused);
        if (!g_stream_paused)
            settings_close();
        move_vibrate(&g_moves[i], 200, 120);
        LOG("stream: %s (%s held)", g_stream_paused ? "lobby" : "back to the game",
            move_index_hand(i) == HAND_LEFT ? "left Cross" : "right Circle");
    }
    if (!any_held)
        fired = false;
}

// START on either Move, or OPTIONS on the DualShock 4, opens / closes the settings in the
// lobby (not while the lobby is black: headset not detected). While streaming, START is
// SteamVR's system button.
static void handle_start_button(uint64_t now)
{
    static bool was_pressed[MOVE_MAX + 1];
    for (int i = 0; i <= MOVE_MAX; i++) {
        const bool pressed = i < MOVE_MAX ? g_moves[i].connected && (g_moves[i].buttons & MOVE_BUTTON_START)
                                          : g_pad.connected && (g_pad.buttons & PAD_BUTTON_OPTIONS);
        if (pressed && !was_pressed[i] && g_lobby_shown && !headset_lost(now) && !settings_is_wizard()) {
            if (settings_is_open()) {
                settings_close();
            } else {
                float p[3], q[4];
                head_pose(p, q);
                settings_open(v3(p[0], p[1], p[2]));
            }
        }
        was_pressed[i] = pressed;
    }
}

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    log_init();
    LOG("%s client v%s starting", ALVR_PS4_TITLE, ALVR_PS4_VERSION);
    detect_ps4_model();

    read_ip();
    init_user();
    config_load(&g_config, g_user_id);
    LOG("PS4 IP address: %s", g_ip);

    Screen screen;
    if (!screen_init(&screen, 1920, 1080)) {
        LOG("screen init failed, running headless");
    }

    probe_modules();
    wait_for_headset(&screen);
    start_headset();

    // Display through the system VR compositor: our status screen becomes the
    // floating 2D screen of the PS4's VR mode.
    char vo_path[128];
    snprintf(vo_path, sizeof(vo_path), "/%s/common/lib/libSceVideoOut.sprx", sceKernelGetFsSandboxRandomWord());
    int videoout_module = (int)sceKernelLoadStartModule(vo_path, 0, nullptr, 0, nullptr, nullptr);
    LOG("libSceVideoOut handle=%d", videoout_module);
    g_display_hz = g_config.refresh_rate == 90 ? 90 : 120;
    if (screen.handle > 0 && g_hmd.handle > 0 && screen_register_vr_buffers(&screen, 2) &&
        screen_set_vr_output_mode(&screen, videoout_module, &g_display_hz) &&
        reproj_start(g_probes[0].handle, screen.handle, 2)) {
        LOG("reprojection started (2D VR mode, %d Hz)", g_display_hz);
        g_paced = reproj_enable_frame_event(g_probes[0].handle);
    }
    else
        LOG("reprojection NOT started, falling back to direct TV output");
    LOG("display %d Hz, stream %d fps (config %d Hz)", g_display_hz, stream_fps(), g_config.refresh_rate);

    bench_start(); // Dev build: video bench server (tools/video_bench.py)

    // The PC is offered the stream rate, known once the output mode is set.
    g_wizard_pending = g_config.user_height_cm <= 0;
    if (g_wizard_pending)
        LOG("first launch: the PC is searched for once the height is confirmed");
    else
        start_alvr();
    LOG("ready");

    unsigned frame = 0;
    for (;;) {
        tracker_update(&g_tracker);
        move_update(g_moves);
        pad_update(&g_pad);
        const uint64_t now = sceKernelGetProcessTime();
        poll_other_users(now);
        update_other_users();
        AlvrStatus alvr_st;
        alvr_get_status(&alvr_st);
        bool pc_connected = alvr_st.state == ALVR_STREAMING;
        update_tracking_state(pc_connected);
        confirm_position_if_stuck(now);
        handle_lobby_toggle(now, pc_connected);
        const bool in_stream = pc_connected && !g_stream_paused; // the game is shown
        update_pad_rumble(in_stream);
        move_set_vibration_strength(g_config.vibration_percent);
        pad_set_vibration_strength(g_config.vibration_percent);
        video_set_frame_period(g_display_hz == 90 ? 1000000 / 90 : 1000000 / 60);
        for (int i = 0; i < MOVE_MAX; i++) {
            WandInput prev = g_wand_emu[i].last;
            wand_update(&g_wand_emu[i], move_index_hand(i), g_moves[i], &g_wand[i]);
            const WandInput &w = g_wand[i];
            // Lobby haptics feedback, only while the game is not shown (then ALVR drives the
            // motors): a tick on pad click and grip, a longer buzz on a full trigger pull
            // (not while the settings are open: the trigger clicks their buttons).
            if (!in_stream && ((w.pad_click && !prev.pad_click) || (w.grip && !prev.grip)))
                move_vibrate(&g_moves[i], 150, 40);
            if (!in_stream && !settings_is_open() && w.trigger >= 0.9f && prev.trigger < 0.9f)
                move_vibrate(&g_moves[i], 220, 120);
            if (w.pad_click != prev.pad_click || w.grip != prev.grip || w.menu != prev.menu ||
                w.system != prev.system || w.trigger_click != prev.trigger_click)
                LOG("wand %s: pad %s%s (%.2f %.2f) grip=%d menu=%d system=%d trigger=%.2f%s",
                    move_index_hand(i) == HAND_LEFT ? "L" : "R", w.pad_touch ? "touch" : "-",
                    w.pad_click ? "+click" : "", w.pad_x, w.pad_y, w.grip, w.menu, w.system, w.trigger,
                    w.trigger_click ? " (click)" : "");
        }
        handle_start_button(now);
        send_alvr_uplink();
        if (screen.handle > 0 && reproj_active()) {
            // 3D lobby once the tracker has given an orientation; a plain 2D screen is only
            // shown before that. Afterwards the lobby keeps the last pose (the quality
            // drops briefly during a tracking reset, which flashed the 2D screen).
            static bool lobby_started = false;
            if (g_tracker.results_ok && g_tracker.orientation_quality != 0)
                lobby_started = true;
            // A video bench clip is shown like a stream (Dev build).
            const bool video_on = in_stream || bench_active();
            if (!video_on)
                video_not_shown();
            bool stereo_video = video_on && render_video();
            if (stereo_video && g_loop_resume_us) {
                // Only from a wake-up of this iteration (a stale one gave a 15 s "work time",
                // hence a 15 s lead: the submission never came before the pass).
                const uint64_t work = sceKernelGetProcessTime() - g_loop_resume_us;
                if (work < g_pass_period_us)
                    g_loop_work_us = work > g_loop_work_us ? work : g_loop_work_us - g_loop_work_us / 64;
            }
            g_loop_resume_us = 0;
            bool stereo = stereo_video;
            g_lobby_shown = false;
            if (!stereo) {
                stereo = lobby_started && render_lobby(&screen);
                g_lobby_shown = stereo;
            }
            if (!g_lobby_shown)
                settings_close(); // the settings only live in the lobby
            if (!stereo) {
                draw_status(&screen, "Starting...");
                static GnmTexture tex[2];
                gnm_texture_linear_bgra(&tex[screen.cur], screen.buffers[screen.cur], screen.width, screen.height,
                                        screen.width);
                reproj_submit_2d(&tex[screen.cur]);
            }
            // Once an app uses the GPU (reprojection, tracker compute) the system relies on
            // sceGnmSubmitDone as its safe point to suspend or close it. Without it, closing
            // the app hangs, then ends in CE-34878-0.
            sceGnmSubmitDone();
            screen.cur ^= 1;
            // Pace to 60 Hz (90 Hz on a 90 Hz headset) from the frame start (the compositor
            // re-displays at 120 Hz).
            // Sleeping a fixed 16 ms after rendering made every frame render + 16 ms long.
            // While streaming, the loop follows the video instead: each frame is handed to
            // the compositor as soon as it is converted (a free-running 60 Hz timer beat
            // against the PC's frame clock, showing some frames twice and skipping others).
            static uint64_t next_us = 0;
            uint64_t now_us = sceKernelGetProcessTime();
            if (next_us == 0 || now_us > next_us + 50000)
                next_us = now_us;
            next_us += 1000000 / (g_display_hz == 90 ? 90 : 60);
            if (stereo_video) {
                // Just before the next compositor pass (paced), or the next converted frame.
                if (!g_paced || !pass_wait_and_lead())
                    video_wait_new(video_published_seq(), 25000);
                next_us = 0;
            } else if (next_us > now_us) {
                sceKernelUsleep((uint32_t)(next_us - now_us));
            }
        } else if (screen.handle > 0) {
            draw_status(&screen, "PlayStation VR not available");
            screen_flip(&screen);
        } else {
            sceKernelUsleep(16000);
        }
        monitor_headset(frame, sceKernelGetProcessTime());
        if (frame % 30 == 0)
            poll_system_events();
        if (frame % 60 == 30 && g_tracker.results_ok) {
            const TrackerPose &p = g_tracker.device_pose;
            LOG("pose q=(%+.4f %+.4f %+.4f %+.4f) p=(%+.4f %+.4f %+.4f) ts=%llu", p.qx, p.qy, p.qz, p.qw,
                p.px, p.py, p.pz, (unsigned long long)g_tracker.timestamp);
        }
        if (frame % 1800 == 0)
            LOG("heartbeat frame=%u", frame);
        frame++;
    }
    return 0;
}
