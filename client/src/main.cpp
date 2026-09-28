// ALVR PS4 client — stage 1: base homebrew.
//
// Shows a status screen on the TV, broadcasts logs over UDP to the PC, and
// probes the system modules the later stages depend on (Hmd, VrTracker, Move,
// Camera, video decoder, audio) so we know what loads from a homebrew process.

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
#include "lobby.h"
#include "config.h"
#include "move.h"
#include "wand.h"
#include "reproj.h"
#include "log.h"
#include "screen.h"
#include "tracker.h"

#define ALVR_PS4_VERSION "0.6.1"

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
static ClientConfig g_config;

static void start_headset()
{
    int rc = sceUserServiceInitialize(nullptr);
    if (rc < 0 && (unsigned)rc != 0x80960003 /* already initialized */)
        LOG("sceUserServiceInitialize -> 0x%08x", (unsigned)rc);
    rc = sceUserServiceGetInitialUser(&g_user_id);
    LOG("initial user id=0x%x (rc=0x%08x)", g_user_id, (unsigned)rc);

    if (hmd_start(g_probes[0].handle, g_user_id, &g_hmd))
        LOG("headset opened, handle=0x%x", g_hmd.handle);
    else
        LOG("headset open FAILED");

    if (tracker_start(g_probes[1].handle, g_probes[4].handle, g_hmd.handle, &g_tracker))
        LOG("tracker started, HMD registered");
    else
        LOG("tracker start FAILED");
    move_start(g_probes[2].handle, g_user_id, g_moves);
    tracker_run_thread();

    // The DualShock 4 is not used: not registered with the tracker, light bar reset
    // to the system's generic colour.
    int rc_pad = scePadInit();
    int pad = scePadOpen(g_user_id, 0, 0, nullptr);
    LOG("scePadInit -> 0x%08x, scePadOpen -> 0x%08x", (unsigned)rc_pad, (unsigned)pad);
    if (pad >= 0)
        LOG("scePadResetLightBar -> 0x%08x", (unsigned)scePadResetLightBar(pad));
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
        TrackedDevice devs[MOVE_MAX];
        for (int i = 0; i < MOVE_MAX; i++)
            devs[i] = g_moves[i].track;
        tracker_recalibrate_all(devs, MOVE_MAX);
    }
    if (st.is_system_ui_overlaid != last.is_system_ui_overlaid ||
        st.is_in_background_execution != last.is_in_background_execution ||
        st.is_out_of_vr_play_area != last.is_out_of_vr_play_area)
        LOG("system status: ui_overlaid=%d background=%d out_of_vr_play_area=%d", st.is_system_ui_overlaid,
            st.is_in_background_execution, st.is_out_of_vr_play_area);
    last = st;
    for (int i = 0; i < st.event_num; i++) {
        static SystemServiceEvent ev;
        if (system_receive_event(&ev) < 0)
            break;
        LOG("system event type=0x%x", ev.type);
    }
}

// Stereo lobby, rendered in software into the side-by-side buffer and handed to the
// system compositor together with the pose it was rendered for.
static bool render_lobby(Screen *s)
{
    static float floor_y = -1.3f;
    static bool floor_set = false;
    const TrackerPose &tp = g_tracker.device_pose;
    if (!floor_set && g_tracker.status == 1 && g_tracker.position_quality == 9) {
        floor_y = tp.py - 1.2f; // seated height guess until a proper floor calibration exists
        floor_set = true;
        LOG("lobby: floor set at y=%.3f (head y=%.3f)", floor_y, tp.py);
    }
    LobbyView view;
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
    const HmdFieldOfView &f = g_hmd.fov;
    view.fov[0] = EyeFov{f.tan_out, f.tan_in, f.tan_top, f.tan_bottom};
    view.fov[1] = EyeFov{f.tan_in, f.tan_out, f.tan_top, f.tan_bottom};
    view.floor_y = floor_y;
    for (int i = 0; i < MOVE_MAX; i++) {
        const MoveController &m = g_moves[i];
        LobbyView::Controller &c = view.controllers[i];
        // Always shown in the lobby; the 10 s "searching" state is only for SteamVR.
        c.visible = m.connected && m.track.has_position;
        c.pos = v3(m.track.position[0], m.track.position[1], m.track.position[2]);
        c.rot = Quat{m.track.orientation[0], m.track.orientation[1], m.track.orientation[2], m.track.orientation[3]};
        c.rgb = move_led_rgb(m.track.led_color);
        c.tracked = m.track.position_quality == 9 || m.track.position_quality == 6;
        const WandInput &w = g_wand[i];
        c.pad_touch = w.pad_touch;
        c.pad_click = w.pad_click;
        c.pad_x = w.pad_x;
        c.pad_y = w.pad_y;
    }
    // The 2 s "searching" state of the headset is only reported to SteamVR.
    view.grey = false;
    // Info panel, once, far in front (towards the camera, 3 m beyond it).
    static char info_lines[5][96];
    snprintf(info_lines[0], sizeof(info_lines[0]), "ALVR PS4");
    snprintf(info_lines[1], sizeof(info_lines[1]), "Waiting for the PC (ALVR streamer %s)", ALVR_STREAMER_VERSION);
    snprintf(info_lines[2], sizeof(info_lines[2]), "Hostname: %s", g_config.hostname);
    snprintf(info_lines[3], sizeof(info_lines[3]), "IP: %s", g_ip);
    snprintf(info_lines[4], sizeof(info_lines[4]), "Client v%s", ALVR_PS4_VERSION);
    for (int i = 0; i < 5; i++)
        view.info[i] = info_lines[i];
    view.info[5] = nullptr;
    view.info_pos = v3(0.0f, floor_y + 2.1f, -3.0f);
    view.info_yaw = 0.0f;
    // Each eye gets its own 960x1080 image with pitch == width: with both eyes in one
    // 1920-wide buffer the compositor ignored the pitch and mixed the eyes row by row.
    static uint32_t *eye_buf[2][2]; // [double-buffer index][eye]
    const int eye_w = 960, eye_h = 1080;
    if (!eye_buf[0][0]) {
        const size_t each = (size_t)eye_w * eye_h * 4, align = 0x10000;
        const size_t total = (each * 4 + align - 1) / align * align;
        off_t phys = 0;
        void *mem = nullptr;
        // WB onion (type 0), CPU-cached: the renderer reads pixels back (AA blending),
        // which is extremely slow on write-combined garlic memory. The GPU reads onion too.
        if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), total, align, 0, &phys) < 0 ||
            sceKernelMapDirectMemory(&mem, total, 0x33, 0, phys, align) < 0) {
            LOG("lobby: eye buffer allocation failed");
            return false;
        }
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
    // Per-eye block for the compositor: tangent -> uv transform,
    //   uv = tangent * scale + offset   (x right, y down),
    // stored as {scale x, scale y, offset x, offset y}. Deduced from three captures
    // (image size varied inversely with the scale, right edge at -0.22 as predicted).
    float fov_block[2][4];
    for (int eye = 0; eye < 2; eye++) {
        const EyeFov &e = view.fov[eye];
        const float w = e.tan_left + e.tan_right, h = e.tan_up + e.tan_down;
        fov_block[eye][0] = 1.0f / w;
        fov_block[eye][1] = 1.0f / h;
        fov_block[eye][2] = e.tan_left / w;
        fov_block[eye][3] = e.tan_up / h;
    }
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

static void draw(Screen *s, unsigned frame)
{
    screen_fill(s, 0x101820);
    screen_rect(s, 0, 0, s->width, 90, 0x1f3a5f);
    screen_text(s, 40, 20, "ALVR PS4 client  v" ALVR_PS4_VERSION, 0xffffff);

    char line[160];
    uint64_t secs = sceKernelGetProcessTime() / 1000000;
    snprintf(line, sizeof(line), "PS4 IP: %s    logs: UDP broadcast port %d    uptime: %llus    frame: %u",
             g_ip, LOG_UDP_PORT, (unsigned long long)secs, frame);
    screen_text(s, 40, 52, line, 0xa0c4ff);

    if (g_hmd.initialized)
        snprintf(line, sizeof(line), "PSVR: %s  handle=0x%x  panel %ux%u", hmd_status_name(g_hmd.info.status),
                 g_hmd.handle, g_hmd.info.panel_width, g_hmd.info.panel_height);
    else
        snprintf(line, sizeof(line), "PSVR: libSceHmd not initialized");
    screen_rect(s, 0, 90, s->width, 34, 0x16283f);
    screen_text(s, 40, 95, line, g_hmd.info.status == HMD_STATUS_READY ? 0x40ff80 : 0xffc040);

    // Heartbeat square: proves the render loop is alive.
    screen_rect(s, s->width - 80, 25, 40, 40, (frame / 30) % 2 ? 0x40ff80 : 0x205030);

    static char lines[LOG_RING_LINES][LOG_LINE_MAX];
    int n = log_snapshot(lines);
    const TrackerPose &pz = g_tracker.device_pose;
    snprintf(line, sizeof(line), "Tracker: %s  status=%s pos=%s orient=%s  q=(%+.3f %+.3f %+.3f %+.3f) p=(%+.3f %+.3f %+.3f)  camera=%d",
             g_tracker.hmd_registered ? "HMD registered" : "off", tracker_status_name(g_tracker.status),
             tracker_quality_name(g_tracker.position_quality), tracker_quality_name(g_tracker.orientation_quality),
             pz.qx, pz.qy, pz.qz, pz.qw, pz.px, pz.py, pz.pz, g_tracker.camera_attached);
    screen_rect(s, 0, 124, s->width, 30, 0x16283f);
    screen_text(s, 40, 127, line, g_tracker.status == 1 ? 0x40ff80 : 0xffc040);

    int y = 164;
    for (int i = 0; i < n && y < s->height - 30; i++, y += 24) {
        uint32_t color = strstr(lines[i], "FAIL") || strstr(lines[i], "MISSING") ? 0xff7070 : 0xd0d0d0;
        screen_text(s, 40, y, lines[i], color);
    }
}

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    log_init();
    LOG("ALVR PS4 client v%s starting", ALVR_PS4_VERSION);

    read_ip();
    config_load(&g_config);
    LOG("PS4 IP address: %s", g_ip);

    Screen screen;
    if (!screen_init(&screen, 1920, 1080)) {
        LOG("screen init failed, running headless");
    }

    probe_modules();
    start_headset();

    // Display through the system VR compositor: our status screen becomes the
    // floating 2D screen of the PS4's VR mode.
    char vo_path[128];
    snprintf(vo_path, sizeof(vo_path), "/%s/common/lib/libSceVideoOut.sprx", sceKernelGetFsSandboxRandomWord());
    int videoout_module = (int)sceKernelLoadStartModule(vo_path, 0, nullptr, 0, nullptr, nullptr);
    LOG("libSceVideoOut handle=%d", videoout_module);
    if (screen.handle > 0 && g_hmd.handle > 0 && screen_register_vr_buffers(&screen, 2) &&
        screen_set_vr_output_mode(&screen, videoout_module) && reproj_start(g_probes[0].handle, screen.handle, 2))
        LOG("reprojection started (2D VR mode)");
    else
        LOG("reprojection NOT started, falling back to direct TV output");
    LOG("ready");

    unsigned frame = 0;
    for (;;) {
        tracker_update(&g_tracker);
        move_update(g_moves);
        for (int i = 0; i < MOVE_MAX; i++) {
            WandInput prev = g_wand_emu[i].last;
            wand_update(&g_wand_emu[i], move_index_hand(i), g_moves[i], &g_wand[i]);
            const WandInput &w = g_wand[i];
            // Lobby haptics feedback (until ALVR drives the motors): a tick on pad click
            // and grip, a longer buzz on a full trigger pull.
            if ((w.pad_click && !prev.pad_click) || (w.grip && !prev.grip))
                move_vibrate(&g_moves[i], 150, 40);
            if (w.trigger_click && !prev.trigger_click)
                move_vibrate(&g_moves[i], 220, 120);
            if (w.pad_click != prev.pad_click || w.grip != prev.grip || w.menu != prev.menu ||
                w.system != prev.system || w.trigger_click != prev.trigger_click)
                LOG("wand %s: pad %s%s (%.2f %.2f) grip=%d menu=%d system=%d trigger=%.2f%s",
                    move_index_hand(i) == HAND_LEFT ? "L" : "R", w.pad_touch ? "touch" : "-",
                    w.pad_click ? "+click" : "", w.pad_x, w.pad_y, w.grip, w.menu, w.system, w.trigger,
                    w.trigger_click ? " (click)" : "");
        }
        if (screen.handle > 0 && reproj_active()) {
            // 3D lobby once the tracker has given an orientation; the 2D status screen is
            // only shown before that. Afterwards the lobby keeps the last pose (the quality
            // drops briefly during a tracking reset, which flashed the 2D screen).
            static bool lobby_started = false;
            if (g_tracker.results_ok && g_tracker.orientation_quality != 0)
                lobby_started = true;
            bool stereo = lobby_started && render_lobby(&screen);
            if (!stereo) {
                draw(&screen, frame);
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
            // Pace to 60 Hz from the frame start (the compositor re-displays at 120 Hz).
            // Sleeping a fixed 16 ms after rendering made every frame render + 16 ms long.
            static uint64_t next_us = 0;
            uint64_t now_us = sceKernelGetProcessTime();
            if (next_us == 0 || now_us > next_us + 50000)
                next_us = now_us;
            next_us += 16667;
            if (next_us > now_us)
                sceKernelUsleep((uint32_t)(next_us - now_us));
        } else if (screen.handle > 0) {
            draw(&screen, frame);
            screen_flip(&screen);
        } else {
            sceKernelUsleep(16000);
        }
        if (frame % 60 == 0 && g_hmd.initialized) {
            uint32_t prev = g_hmd.info.status;
            hmd_refresh(&g_hmd);
            if (g_hmd.info.status != prev)
                LOG("HMD status changed: %s -> %s", hmd_status_name(prev), hmd_status_name(g_hmd.info.status));
        }
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
