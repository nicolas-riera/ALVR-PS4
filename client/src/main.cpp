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
#include <orbis/UserService.h>

#include "hmd.h"
#include "lobby.h"
#include "reproj.h"
#include "log.h"
#include "screen.h"
#include "tracker.h"

#define ALVR_PS4_VERSION "0.4.4 (stage 3: 3D lobby)"

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
    tracker_run_thread();
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
    view.head_pos = v3(tp.px, tp.py, tp.pz);
    view.head_rot = Quat{tp.qx, tp.qy, tp.qz, tp.qw};
    view.ipd = 0.063f;
    const HmdFieldOfView &f = g_hmd.fov;
    view.fov[0] = EyeFov{f.tan_out, f.tan_in, f.tan_top, f.tan_bottom};
    view.fov[1] = EyeFov{f.tan_in, f.tan_out, f.tan_top, f.tan_bottom};
    view.floor_y = floor_y;
    // Each eye gets its own 960x1080 image with pitch == width: with both eyes in one
    // 1920-wide buffer the compositor ignored the pitch and mixed the eyes row by row.
    static uint32_t *eye_buf[2][2]; // [double-buffer index][eye]
    const int eye_w = 960, eye_h = 1080;
    if (!eye_buf[0][0]) {
        const size_t each = (size_t)eye_w * eye_h * 4, align = 0x10000;
        const size_t total = (each * 4 + align - 1) / align * align;
        off_t phys = 0;
        void *mem = nullptr;
        if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), total, align, 3, &phys) < 0 ||
            sceKernelMapDirectMemory(&mem, total, 0x33, 0, phys, align) < 0) {
            LOG("lobby: eye buffer allocation failed");
            return false;
        }
        for (int i = 0; i < 4; i++)
            eye_buf[i / 2][i % 2] = (uint32_t *)((char *)mem + each * i);
    }
    static int cur = 0;
    static GnmTexture eye_tex[2][2];
    for (int eye = 0; eye < 2; eye++) {
        lobby_render_eye(eye_buf[cur][eye], eye_w, eye_h, eye_w, &view, eye);
        gnm_texture_linear_bgra(&eye_tex[cur][eye], eye_buf[cur][eye], eye_w, eye_h, eye_w);
    }
    // Per-eye block for the compositor: the texture's extent in tangent space as
    // {half width, half height, centre x, centre y} (same scale/offset form as the
    // 2D screen's {1, 1, 0, 0}). Raw edge tangents here put the image in a corner.
    float fov_block[2][4];
    for (int eye = 0; eye < 2; eye++) {
        const EyeFov &e = view.fov[eye];
        fov_block[eye][0] = (e.tan_left + e.tan_right) * 0.5f;
        fov_block[eye][1] = (e.tan_up + e.tan_down) * 0.5f;
        fov_block[eye][2] = (e.tan_right - e.tan_left) * 0.5f;
        fov_block[eye][3] = (e.tan_up - e.tan_down) * 0.5f;
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
    bool ok = reproj_submit_stereo(&eye_tex[cur][0], &eye_tex[cur][1], fov_block[0], fov_block[1], &pose) == 0;
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
        if (screen.handle > 0 && reproj_active()) {
            // 3D lobby once the tracker has an orientation, 2D status screen before that.
            bool stereo = g_tracker.results_ok && g_tracker.orientation_quality != 0 && render_lobby(&screen);
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
            sceKernelUsleep(16000); // ~60 Hz; the compositor re-displays at the headset rate
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
