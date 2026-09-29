#include "tracker.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include <orbis/libkernel.h>

#include "camera.h"
#include "log.h"

// Call sequence and parameter values follow Beat Saber's PSVR subsystem
// (reference/decomp/beatsaber_psvr.c: psvr_init, psvr_tracker_frame, psvr_get_result).

enum : uint32_t {
    DEVICE_HMD = 0,
    PROFILE_000 = 0,
    PROFILE_100 = 100,
    EXEC_SERIAL = 0,
    EXEC_PARALLEL = 1,
    RESULT_PREDICTED = 0,
    RESULT_RAW = 1,
    ORIENTATION_ABSOLUTE = 0,
};

struct CalibrationSettings {
    uint32_t hmd_position, pad_position, move_position, gun_position;
    uint32_t reserved[4];
};

struct QueryMemoryParam {
    uint32_t size;
    uint32_t profile;
    uint32_t reserved[6];
    CalibrationSettings calibration_settings;
};

struct QueryMemoryResult {
    uint32_t size;
    uint32_t onion_size, onion_alignment;
    uint32_t garlic_size, garlic_alignment;
    uint32_t work_size, work_alignment;
    uint32_t reserved[9];
};

struct InitParam {
    uint32_t size;
    uint32_t profile;
    uint32_t execution_mode;
    int32_t hmd_thread_priority, pad_thread_priority, move_thread_priority, gun_thread_priority;
    int32_t reserved;
    uint64_t cpu_mask;
    CalibrationSettings calibration_settings;
    void *onion;
    uint32_t onion_size, onion_alignment;
    void *garlic;
    uint32_t garlic_size, garlic_alignment;
    void *work;
    uint32_t work_size, work_alignment;
    int32_t gpu_pipe_id;
    int32_t gpu_queue_id;
};

struct UpdateMotionSensorDataParam {
    uint32_t size;
    uint32_t device_type;
    uint32_t operation_mode; // 0 = by device type (handle must be 0), 1 = by handle
    int32_t handle;
    int32_t reserved[4];
};

struct GetResultParam {
    uint32_t size;
    int32_t handle;
    uint32_t result_type;
    uint32_t reserved0;
    uint64_t prediction_time;
    uint32_t orientation_type;
    uint32_t reserved1;
    uint32_t usage_type;
    uint32_t user_frame_number;
    uint32_t debug_marker_type;
    uint32_t reserved2[2];
};

struct GpuSubmitParam {
    uint32_t size;
    uint32_t pad_tracking_preference;
    uint32_t camera_meta_check_mode;
    uint32_t tracking_device_permit_type;
    uint32_t robustness_level;
    uint32_t reserved0[10];
    uint32_t reserved1;
    uint8_t camera_frame_data[CAMERA_FRAME_DATA_SIZE];
};

// GpuWait / CpuProcess parameters: size 0x20, everything else zero.
struct SmallParam {
    uint32_t size;
    uint32_t operation_mode;
    int32_t handle;
    uint32_t reserved[5];
};

struct ResultData {
    int32_t handle;
    uint32_t connected;
    uint32_t reserved0[2];
    uint64_t timestamp;
    uint64_t device_timestamp;
    uint32_t recalibrate_necessity;
    uint32_t playarea_brightness_risk;
    uint32_t reserved1[2];
    uint32_t led_color;
    uint32_t status;
    uint32_t position_quality;
    uint32_t orientation_quality;
    float velocity[3];
    float acceleration[3];
    float angular_velocity[3];
    float angular_acceleration[3];
    float camera_orientation[4];
    // HMD info (first member of the HMD/pad/move union at +0x80).
    TrackerPose device_pose;
    TrackerPose left_eye_pose;
    TrackerPose right_eye_pose;
    TrackerPose head_pose;
    // ...rest of the (large) structure is not read yet.
};

// Sizes checked by libSceVrTracker / used by Beat Saber.
static_assert(sizeof(InitParam) == 0x80, "InitParam size");
static_assert(sizeof(TrackerPose) == 0x40, "TrackerPose size");
static_assert(__builtin_offsetof(ResultData, device_pose) == 0x80, "ResultData device_pose");
static_assert(sizeof(UpdateMotionSensorDataParam) == 0x20, "UpdateMotionSensorDataParam size");
static_assert(sizeof(GetResultParam) == 0x38, "GetResultParam size");
static_assert(sizeof(GpuSubmitParam) == 0x288, "GpuSubmitParam size");
static_assert(sizeof(SmallParam) == 0x20, "SmallParam size");

typedef int (*PFN_QueryMemory)(const QueryMemoryParam *, QueryMemoryResult *);
typedef int (*PFN_Init)(const InitParam *);
typedef int (*PFN_RegisterDevice)(uint32_t device_type, int32_t handle);
typedef int (*PFN_UpdateMotionSensorData)(const UpdateMotionSensorDataParam *);
typedef int (*PFN_GetResult)(const GetResultParam *, void *result);
typedef int (*PFN_GetTime)(uint64_t *);
typedef int (*PFN_GpuSubmit)(const GpuSubmitParam *);
typedef int (*PFN_SmallCall)(const SmallParam *);
typedef int (*PFN_CameraIsAttached)(int index);

static PFN_UpdateMotionSensorData p_update_motion;
static PFN_GetResult p_get_result;
static PFN_GetTime p_get_time;
static PFN_GpuSubmit p_gpu_submit;
static PFN_SmallCall p_gpu_wait;
static PFN_SmallCall p_cpu_process;
static int g_hmd_handle;
static CameraState g_camera = {-1, 0};
static TrackerState *g_state;
static int g_tracker_module = -1;
static volatile bool g_stop;
static pthread_t g_thread;
static bool g_thread_started;

// The real ResultData is 0x5f0 bytes; give the library ample room.
alignas(16) static uint8_t g_result_buf[16384];

const char *tracker_status_name(uint32_t s)
{
    switch (s) {
    case 0: return "NOT_STARTED";
    case 1: return "TRACKING";
    case 2: return "NOT_TRACKING";
    case 3: return "CALIBRATING";
    default: return "?";
    }
}

const char *tracker_quality_name(uint32_t q)
{
    switch (q) {
    case 0: return "NONE";
    case 3: return "NOT_VISIBLE";
    case 6: return "PARTIAL";
    case 9: return "FULL";
    default: return "?";
    }
}

static void *resolve(int module, const char *name)
{
    void *fn = nullptr;
    if (sceKernelDlsym(module, name, &fn) != 0 || !fn) {
        LOG("tracker: symbol %s not found", name);
        return nullptr;
    }
    return fn;
}

static void *alloc_direct(size_t size, size_t align, int mem_type, const char *what)
{
    size = (size + align - 1) / align * align;
    off_t phys = 0;
    int rc = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), size, align, mem_type, &phys);
    if (rc < 0) {
        LOG("tracker: alloc %s (%zu bytes) failed 0x%08x", what, size, (unsigned)rc);
        return nullptr;
    }
    void *ptr = nullptr;
    // CPU read/write + GPU read/write
    rc = sceKernelMapDirectMemory(&ptr, size, 0x33, 0, phys, align);
    if (rc < 0) {
        LOG("tracker: map %s failed 0x%08x", what, (unsigned)rc);
        return nullptr;
    }
    return ptr;
}

bool tracker_start(int module, int camera_module, int hmd_handle, TrackerState *st)
{
    memset(st, 0, sizeof(*st));
    st->camera_attached = -1;
    g_state = st;
    if (module < 0 || hmd_handle <= 0)
        return false;

    g_tracker_module = module;
    auto query = (PFN_QueryMemory)resolve(module, "sceVrTrackerQueryMemory");
    auto init = (PFN_Init)resolve(module, "sceVrTrackerInit");
    auto reg = (PFN_RegisterDevice)resolve(module, "sceVrTrackerRegisterDevice");
    p_update_motion = (PFN_UpdateMotionSensorData)resolve(module, "sceVrTrackerUpdateMotionSensorData");
    p_get_result = (PFN_GetResult)resolve(module, "sceVrTrackerGetResult");
    p_get_time = (PFN_GetTime)resolve(module, "sceVrTrackerGetTime");
    p_gpu_submit = (PFN_GpuSubmit)resolve(module, "sceVrTrackerGpuSubmit");
    p_gpu_wait = (PFN_SmallCall)resolve(module, "sceVrTrackerGpuWait");
    p_cpu_process = (PFN_SmallCall)resolve(module, "sceVrTrackerCpuProcess");
    if (!query || !init || !reg || !p_update_motion || !p_get_result || !p_get_time || !p_gpu_submit ||
        !p_gpu_wait || !p_cpu_process)
        return false;

    // Same order as the games: camera first, then the tracker.
    if (camera_module >= 0) {
        auto is_attached = (PFN_CameraIsAttached)resolve(camera_module, "sceCameraIsAttached");
        if (is_attached) {
            st->camera_attached = is_attached(0);
            LOG("sceCameraIsAttached(0) -> %d", st->camera_attached);
        }
        if (st->camera_attached == 1 && !camera_start(camera_module, &g_camera))
            LOG("camera start FAILED");
    }

    // Tracker profile must match the camera config type (5 -> 100, 4 -> 0).
    const uint32_t profile = g_camera.config_type == 4 ? PROFILE_000 : PROFILE_100;
    LOG("tracker profile %u", profile);

    // Calibration: HMD manual (its only option), DualShock 4 and Move automatic.
    CalibrationSettings calib;
    memset(&calib, 0, sizeof(calib));
    calib.pad_position = 1;
    calib.move_position = 1;

    QueryMemoryParam qp;
    memset(&qp, 0, sizeof(qp));
    qp.size = sizeof(qp);
    qp.profile = profile;
    qp.calibration_settings = calib;
    QueryMemoryResult qr;
    memset(&qr, 0, sizeof(qr));
    qr.size = sizeof(qr);
    int rc = query(&qp, &qr);
    LOG("sceVrTrackerQueryMemory -> 0x%08x onion=0x%x/%x garlic=0x%x/%x work=0x%x/%x", (unsigned)rc,
        qr.onion_size, qr.onion_alignment, qr.garlic_size, qr.garlic_alignment, qr.work_size,
        qr.work_alignment);
    if (rc < 0)
        return false;

    void *onion = alloc_direct(qr.onion_size, qr.onion_alignment, 0 /* WB onion */, "onion");
    void *garlic = alloc_direct(qr.garlic_size, qr.garlic_alignment, 3 /* WC garlic */, "garlic");
    void *work = alloc_direct(qr.work_size, qr.work_alignment, 0, "work");
    if (!onion || !garlic || !work)
        return false;

    InitParam ip;
    memset(&ip, 0, sizeof(ip));
    ip.size = sizeof(ip);
    ip.profile = profile;
    ip.execution_mode = EXEC_PARALLEL;
    ip.hmd_thread_priority = ip.pad_thread_priority = ip.move_thread_priority = ip.gun_thread_priority = 256;
    ip.cpu_mask = 0x38; // cores 3-5
    ip.calibration_settings = calib;
    ip.onion = onion;
    ip.onion_size = qr.onion_size;
    ip.onion_alignment = qr.onion_alignment;
    ip.garlic = garlic;
    ip.garlic_size = qr.garlic_size;
    ip.garlic_alignment = qr.garlic_alignment;
    ip.work = work;
    ip.work_size = qr.work_size;
    ip.work_alignment = qr.work_alignment;
    ip.gpu_pipe_id = 4;
    ip.gpu_queue_id = 4;
    rc = init(&ip);
    LOG("sceVrTrackerInit(parallel, prio 256, mask 0x38, pipe 4, queue 4) -> 0x%08x", (unsigned)rc);
    if (rc < 0)
        return false;
    st->initialized = true;

    rc = reg(DEVICE_HMD, hmd_handle);
    LOG("sceVrTrackerRegisterDevice(HMD, 0x%x) -> 0x%08x", hmd_handle, (unsigned)rc);
    st->hmd_registered = rc >= 0;
    g_hmd_handle = hmd_handle;
    st->last_result_rc = st->last_motion_rc = 1; // force first log
    return st->hmd_registered;
}

static void log_on_change(const char *what, int rc, int *last)
{
    if (rc != *last) {
        LOG("%s -> 0x%08x", what, (unsigned)rc);
        *last = rc;
    }
}

// Per camera frame: GpuSubmit -> GpuWait -> CpuProcess. Without a camera frame,
// feed the motion sensors only, as the games do.
static void *tracker_thread(void *)
{
    static GpuSubmitParam submit;
    int last_frame = 1, last_submit = 1, last_wait = 1, last_cpu = 1;
    unsigned frames = 0;
    while (!g_stop) {
        int rc = g_camera.handle >= 0 ? camera_get_frame(&g_camera, submit.camera_frame_data) : -1;
        log_on_change("sceCameraGetFrameData", rc, &last_frame);
        if (rc == 0) {
            submit.size = sizeof(submit);
            rc = p_gpu_submit(&submit);
            if ((unsigned)rc == 0x81260811) {
                // ALREADY_PROCESSING_CAMERA_FRAME: GetFrameData handed back the frame we
                // already submitted. Wait for the next one (camera runs at 60 Hz).
                sceKernelUsleep(1000);
                continue;
            }
            log_on_change("sceVrTrackerGpuSubmit", rc, &last_submit);
            if (rc == 0) {
                SmallParam wp;
                memset(&wp, 0, sizeof(wp));
                wp.size = sizeof(wp);
                rc = p_gpu_wait(&wp);
                log_on_change("sceVrTrackerGpuWait", rc, &last_wait);
                if (rc == 0) {
                    SmallParam cp;
                    memset(&cp, 0, sizeof(cp));
                    cp.size = sizeof(cp);
                    rc = p_cpu_process(&cp);
                    log_on_change("sceVrTrackerCpuProcess", rc, &last_cpu);
                    if (++frames % 600 == 1)
                        LOG("tracker: %u camera frames processed", frames);
                }
                continue;
            }
        }
        // No camera frame: feed motion sensors of every device type, as the games do.
        for (uint32_t type = 0; type <= TRACKER_DEVICE_MOVE; type++) {
            UpdateMotionSensorDataParam mp;
            memset(&mp, 0, sizeof(mp));
            mp.size = sizeof(mp);
            mp.device_type = type;
            rc = p_update_motion(&mp);
            if (type == DEVICE_HMD)
                log_on_change("sceVrTrackerUpdateMotionSensorData(HMD)", rc, &g_state->last_motion_rc);
        }
        sceKernelUsleep(2000);
    }
    return nullptr;
}

void tracker_run_thread()
{
    if (!g_state || !g_state->hmd_registered)
        return;
    int rc = pthread_create(&g_thread, nullptr, tracker_thread, nullptr);
    g_thread_started = rc == 0;
    LOG("tracker thread create -> %d", rc);
}

void tracker_update(TrackerState *st)
{
    if (!st->hmd_registered)
        return;

    GetResultParam gp;
    memset(&gp, 0, sizeof(gp));
    gp.size = sizeof(gp);
    gp.handle = g_hmd_handle;
    gp.result_type = RESULT_PREDICTED;
    gp.orientation_type = ORIENTATION_ABSOLUTE;
    p_get_time(&gp.prediction_time);
    memset(g_result_buf, 0, sizeof(g_result_buf));
    int rc = p_get_result(&gp, g_result_buf);
    log_on_change("sceVrTrackerGetResult(HMD)", rc, &st->last_result_rc);
    if (rc < 0)
        return;

    const ResultData *r = (const ResultData *)g_result_buf;
    if (r->position_quality == 9 || r->position_quality == 6)
        st->last_seen_us = sceKernelGetProcessTime();
    if (r->status != st->status || r->position_quality != st->position_quality ||
        r->orientation_quality != st->orientation_quality || r->led_color != st->led_color)
        LOG("tracker: status=%s pos=%s orient=%s led_color=%u connected=%u", tracker_status_name(r->status),
            tracker_quality_name(r->position_quality), tracker_quality_name(r->orientation_quality),
            r->led_color, r->connected);
    st->status = r->status;
    st->position_quality = r->position_quality;
    st->orientation_quality = r->orientation_quality;
    st->led_color = r->led_color;
    st->device_pose = r->device_pose;
    st->eye_pose[0] = r->left_eye_pose;
    st->eye_pose[1] = r->right_eye_pose;
    st->head_pose = r->head_pose;
    st->timestamp = r->timestamp;
    st->results_ok++;
}

void tracker_stop(int module, int camera_module)
{
    if (!g_state)
        return;
    // Stop feeding frames first, then unregister, terminate, and release the camera.
    g_stop = true;
    if (g_thread_started) {
        pthread_join(g_thread, nullptr);
        g_thread_started = false;
    }
    if (g_state->hmd_registered) {
        auto unreg = (int (*)(int32_t))resolve(module, "sceVrTrackerUnregisterDevice");
        if (unreg)
            LOG("sceVrTrackerUnregisterDevice(HMD) -> 0x%08x", (unsigned)unreg(g_hmd_handle));
        g_state->hmd_registered = false;
    }
    if (g_state->initialized) {
        auto term = (int (*)())resolve(module, "sceVrTrackerTerm");
        if (term)
            LOG("sceVrTrackerTerm -> 0x%08x", (unsigned)term());
        g_state->initialized = false;
    }
    camera_stop(camera_module, &g_camera);
}

bool tracker_reregister_hmd(int hmd_handle)
{
    if (!g_state || !g_state->initialized || g_tracker_module < 0)
        return false;
    auto unreg = (int (*)(int32_t))resolve(g_tracker_module, "sceVrTrackerUnregisterDevice");
    auto reg = (PFN_RegisterDevice)resolve(g_tracker_module, "sceVrTrackerRegisterDevice");
    if (!unreg || !reg)
        return false;
    if (g_state->hmd_registered)
        LOG("sceVrTrackerUnregisterDevice(HMD 0x%x) -> 0x%08x", g_hmd_handle, (unsigned)unreg(g_hmd_handle));
    int rc = reg(DEVICE_HMD, hmd_handle);
    LOG("sceVrTrackerRegisterDevice(HMD, 0x%x) -> 0x%08x", hmd_handle, (unsigned)rc);
    g_hmd_handle = hmd_handle;
    g_state->hmd_registered = rc >= 0;
    return rc >= 0;
}

bool tracker_register_device(TrackedDevice *d, uint32_t type, int handle)
{
    memset(d, 0, sizeof(*d));
    d->handle = handle;
    d->type = type;
    d->last_rc = 1;
    d->orientation[3] = 1.0f;
    if (!g_state || !g_state->initialized || g_tracker_module < 0 || handle < 0)
        return false;
    auto reg = (PFN_RegisterDevice)resolve(g_tracker_module, "sceVrTrackerRegisterDevice");
    if (!reg)
        return false;
    int rc = reg(type, handle);
    LOG("sceVrTrackerRegisterDevice(type %u, 0x%x) -> 0x%08x", type, handle, (unsigned)rc);
    d->registered = rc >= 0;
    return d->registered;
}

void tracker_unregister_device(TrackedDevice *d)
{
    if (!d->registered)
        return;
    auto unreg = (int (*)(int32_t))resolve(g_tracker_module, "sceVrTrackerUnregisterDevice");
    if (unreg)
        LOG("sceVrTrackerUnregisterDevice(0x%x) -> 0x%08x", d->handle, (unsigned)unreg(d->handle));
    d->registered = false;
}

volatile uint32_t g_tracker_controller_prediction_us = 0;

static void qmul(const float a[4], const float b[4], float out[4]) // x, y, z, w
{
    out[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    out[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    out[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    out[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}

static void qrotate(const float q[4], const float v[3], float out[3])
{
    // v + 2w (u x v) + 2 u x (u x v), u = q.xyz
    float t[3] = {2 * (q[1] * v[2] - q[2] * v[1]), 2 * (q[2] * v[0] - q[0] * v[2]), 2 * (q[0] * v[1] - q[1] * v[0])};
    out[0] = v[0] + q[3] * t[0] + (q[1] * t[2] - q[2] * t[1]);
    out[1] = v[1] + q[3] * t[1] + (q[2] * t[0] - q[0] * t[2]);
    out[2] = v[2] + q[3] * t[2] + (q[0] * t[1] - q[1] * t[0]);
}

// SteamVR extrapolates the controllers with their angular velocity in the world frame.
// Whether the tracker reports it in the world or the device frame is decided from real
// motion: the rotation between two successive orientations, q_now * conj(q_prev), is a
// world-frame rotation; both interpretations are correlated with it over fast turns.
static void update_angular_velocity(TrackedDevice *d, const ResultData *r)
{
    const float *w = r->angular_velocity;
    float world[3] = {0, 0, 0};
    if (r->orientation_quality != 0) {
        if (d->angular_frame == 1)
            memcpy(world, w, sizeof(world));
        else if (d->angular_frame == 2)
            qrotate(d->orientation, w, world);
        if (d->angular_frame == 0 && d->prev_q_ts && r->timestamp > d->prev_q_ts) {
            float dt = (r->timestamp - d->prev_q_ts) / 1e6f;
            if (dt > 0.004f && dt < 0.04f) {
                float inv[4] = {-d->prev_q[0], -d->prev_q[1], -d->prev_q[2], d->prev_q[3]}, dq[4];
                qmul(d->orientation, inv, dq);
                float sgn = dq[3] < 0 ? -2.0f / dt : 2.0f / dt;
                float fd[3] = {dq[0] * sgn, dq[1] * sgn, dq[2] * sgn};
                float e = fd[0] * fd[0] + fd[1] * fd[1] + fd[2] * fd[2];
                if (e > 2.0f * 2.0f) { // turning faster than 2 rad/s
                    float local[3];
                    qrotate(d->orientation, w, local);
                    d->corr_world += fd[0] * w[0] + fd[1] * w[1] + fd[2] * w[2];
                    d->corr_local += fd[0] * local[0] + fd[1] * local[1] + fd[2] * local[2];
                    d->corr_energy += e;
                }
                if (d->corr_energy > 500.0f) {
                    float cw = d->corr_world / d->corr_energy, cl = d->corr_local / d->corr_energy;
                    d->angular_frame = cw > 0.5f && cw >= cl ? 1 : cl > 0.5f ? 2 : 3;
                    LOG("device 0x%x: angular velocity frame = %s (correlation world %.2f, local %.2f)", d->handle,
                        d->angular_frame == 1 ? "world" : d->angular_frame == 2 ? "local" : "unusable, not sent", cw, cl);
                }
            }
        }
        memcpy(d->prev_q, d->orientation, sizeof(d->prev_q));
        d->prev_q_ts = r->timestamp;
    }
    memcpy(d->angular_velocity, world, sizeof(world));
}

void tracker_update_device(TrackedDevice *d)
{
    if (!d->registered)
        return;
    GetResultParam gp;
    memset(&gp, 0, sizeof(gp));
    gp.size = sizeof(gp);
    gp.handle = d->handle;
    gp.result_type = RESULT_PREDICTED;
    gp.orientation_type = ORIENTATION_ABSOLUTE;
    p_get_time(&gp.prediction_time);
    gp.prediction_time += g_tracker_controller_prediction_us;
    alignas(16) static uint8_t buf[16384];
    memset(buf, 0, sizeof(buf));
    int rc = p_get_result(&gp, buf);
    if (rc != d->last_rc) {
        LOG("sceVrTrackerGetResult(0x%x) -> 0x%08x", d->handle, (unsigned)rc);
        d->last_rc = rc;
    }
    if (rc < 0)
        return;
    const ResultData *r = (const ResultData *)buf;
    if (r->status != d->status || r->position_quality != d->position_quality ||
        r->orientation_quality != d->orientation_quality || r->led_color != d->led_color)
        LOG("device 0x%x: status=%s pos=%s orient=%s led_color=%u", d->handle, tracker_status_name(r->status),
            tracker_quality_name(r->position_quality), tracker_quality_name(r->orientation_quality), r->led_color);
    d->status = r->status;
    d->position_quality = r->position_quality;
    d->orientation_quality = r->orientation_quality;
    d->led_color = r->led_color;
    d->timestamp = r->timestamp;
    if (r->position_quality == 9 || r->position_quality == 6)
        d->last_seen_us = sceKernelGetProcessTime();
    const TrackerPose &p = r->device_pose;
    if (r->position_quality != 0) {
        d->position[0] = p.px;
        d->position[1] = p.py;
        d->position[2] = p.pz;
        d->has_position = true;
    }
    if (r->orientation_quality != 0) {
        d->orientation[0] = p.qx;
        d->orientation[1] = p.qy;
        d->orientation[2] = p.qz;
        d->orientation[3] = p.qw;
        d->has_orientation = true;
    }
    for (int i = 0; i < 3; i++)
        d->velocity[i] = r->position_quality != 0 ? r->velocity[i] : 0.0f;
    update_angular_velocity(d, r);
}

struct RecalibrateParam {
    uint32_t size; // 0x20
    uint32_t device_type;
    uint32_t calibration_type; // 0 = position, 2 = all
    uint32_t reserved[5];
};
static_assert(sizeof(RecalibrateParam) == 0x20, "RecalibrateParam size");

static void recalibrate(uint32_t type)
{
    auto fn = (int (*)(const RecalibrateParam *))resolve(g_tracker_module, "sceVrTrackerRecalibrate");
    if (!fn)
        return;
    RecalibrateParam p;
    memset(&p, 0, sizeof(p));
    p.size = sizeof(p);
    p.device_type = type;
    p.calibration_type = 2; // as Beat Saber does for a full reset
    int rc = fn(&p);
    if (rc < 0) {
        p.calibration_type = 0;
        int rc2 = fn(&p);
        LOG("sceVrTrackerRecalibrate(type %u): all -> 0x%08x, position -> 0x%08x", type, (unsigned)rc, (unsigned)rc2);
    } else {
        LOG("sceVrTrackerRecalibrate(type %u, all) -> 0x%08x", type, (unsigned)rc);
    }
}

void tracker_recalibrate_all(TrackedDevice *devices, int count)
{
    if (!g_state || !g_state->initialized)
        return;
    recalibrate(DEVICE_HMD);
    bool move_done = false;
    for (int i = 0; i < count; i++)
        if (devices[i].registered && devices[i].type == TRACKER_DEVICE_MOVE && !move_done) {
            recalibrate(TRACKER_DEVICE_MOVE);
            move_done = true;
        }
}
