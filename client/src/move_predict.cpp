#include "move_predict.h"

#include <math.h>
#include <string.h>

// Glide back to the tracked position: the offset left at the regain shrinks with this time
// constant at rest, divided by (1 + speed / GLIDE_SPEED_REF): a controller moving fast
// catches up within a few frames, one held still slides over ~0.3 s (the Quest 2 feel).
// The speed counts up to GLIDE_SPEED_MAX (a velocity spike at the regain must not snap).
static const float GLIDE_TAU_S = 0.15f, GLIDE_SPEED_REF = 0.4f, GLIDE_SPEED_MAX = 3.0f;
// From the tracker's estimate to the arm model when it takes over.
static const float ARM_BLEND_TAU_S = 0.25f;
// Offsets below this are dropped (no endless sub-millimetre glide).
static const float SNAP_M = 0.005f;

static float len3(Vec3 v) { return sqrtf(v.x * v.x + v.y * v.y + v.z * v.z); }

// Headset yaw (radians; 0 = facing -Z, positive turning left), from its forward made
// horizontal (looking up or down, the forward minus its vertical share of the up vector
// still points ahead). False when it cannot tell.
static bool head_yaw(Quat q, float *yaw)
{
    const Vec3 f = rotate(q, v3(0, 0, -1)), u = rotate(q, v3(0, 1, 0));
    const float fx = f.x - u.x * f.y, fz = f.z - u.z * f.y;
    if (fx * fx + fz * fz < 1e-8f)
        return false;
    *yaw = atan2f(-fx, -fz);
    return true;
}

// Rotation by yaw about +Y.
static Vec3 yaw_rotate(float yaw, Vec3 v)
{
    const float c = cosf(yaw), s = sinf(yaw);
    return v3(c * v.x + s * v.z, v.y, -s * v.x + c * v.z);
}

static float wrap_pi(float a)
{
    while (a > 3.14159265f)
        a -= 6.28318531f;
    while (a < -3.14159265f)
        a += 6.28318531f;
    return a;
}

// Body yaw follows the head's lazily: it stays put while the head turns less than
// MOVE_PREDICT_BODY_YAW_RAD away from it (looking aside does not swing the hidden hands).
static void update_body(MovePredict *p, const MovePredictInput &in)
{
    float yaw;
    if (!in.head_valid || !head_yaw(in.head_rot, &yaw))
        return;
    if (!p->body_valid) {
        p->body_yaw = yaw;
        p->body_valid = true;
        return;
    }
    const float d = wrap_pi(yaw - p->body_yaw);
    if (d > MOVE_PREDICT_BODY_YAW_RAD)
        p->body_yaw = wrap_pi(p->body_yaw + d - MOVE_PREDICT_BODY_YAW_RAD);
    else if (d < -MOVE_PREDICT_BODY_YAW_RAD)
        p->body_yaw = wrap_pi(p->body_yaw + d + MOVE_PREDICT_BODY_YAW_RAD);
}

static Vec3 forearm(Quat q) { return rotate(q, v3(0, 0, -MOVE_PREDICT_FOREARM_M)); }

// Arm model position: elbow fixed to the body, forearm along the controller.
static Vec3 arm_position(const MovePredict *p, const MovePredictInput &in)
{
    return in.head + yaw_rotate(p->body_yaw, p->elbow_body) + forearm(in.orientation);
}

static void decay(Vec3 *offset, float dt, float tau)
{
    *offset = *offset * expf(-dt / tau);
    if (len3(*offset) < SNAP_M)
        *offset = v3(0, 0, 0);
}

static void set_mode(MovePredict *p, int mode)
{
    if (p->mode != mode && p->changed_from < 0)
        p->changed_from = p->mode;
    p->mode = mode;
}

void move_predict_reset(MovePredict *p)
{
    memset(p, 0, sizeof(*p));
    p->mode = MOVE_PREDICT_NONE;
    p->changed_from = -1;
}

// Resting state from the motion sensors, smoothed over ~0.3 s: a controller in hand
// always shakes and turns a little (5-10 degrees/s held still in the 2026-10-02
// recordings), one put down does not. Entering and leaving need different thresholds
// (hysteresis), so the sensors' noise does not flip it.
static void update_rest(MovePredict *p, const MovePredictInput &in, float dt)
{
    p->rest_changed = false;
    const float k = dt > 0.0f ? 1.0f - expf(-dt / 0.3f) : 0.0f;
    if (p->prev_rot_valid && dt > 0.0f) {
        const Quat a = p->prev_rot, b = in.orientation;
        float d = fabsf(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
        d = d > 1.0f ? 1.0f : d;
        const float rate = 2.0f * acosf(d) / dt;
        p->rot_rate += (rate - p->rot_rate) * k;
    }
    p->prev_rot = in.orientation;
    p->prev_rot_valid = true;
    if (in.has_accel) {
        if (!p->accel_valid) {
            p->accel_mean = in.accel;
            p->accel_var = 0.0f;
            p->accel_valid = true;
        } else {
            const Vec3 d = in.accel - p->accel_mean;
            p->accel_mean = p->accel_mean + d * k;
            p->accel_var += (d.x * d.x + d.y * d.y + d.z * d.z - p->accel_var) * k;
        }
        const float g = len3(p->accel_mean);
        p->accel_shake = g > 1e-6f ? sqrtf(p->accel_var) / g : 1.0f;
    } else {
        p->accel_shake = 1.0f; // no sensor data: never taken as put down
    }
    const float gyro = in.has_accel ? len3(in.gyro) : 1e9f;
    const bool still = p->accel_shake < MOVE_PREDICT_REST_SHAKE;
    const bool quiet = p->accel_shake < MOVE_PREDICT_QUIET_SHAKE && gyro < MOVE_PREDICT_QUIET_GYRO;
    const bool moving = p->accel_shake > MOVE_PREDICT_MOVE_SHAKE || gyro > MOVE_PREDICT_MOVE_GYRO;
    p->still_since = still ? (p->still_since ? p->still_since : in.now_us) : 0;
    p->moving_since = moving ? (p->moving_since ? p->moving_since : in.now_us) : 0;
    const float head_dist = in.head_valid ? len3(in.head - in.position) : 0.0f;
    if (!quiet || !in.head_valid) {
        p->quiet_since = 0;
    } else if (!p->quiet_since) {
        p->quiet_since = in.now_us;
        p->quiet_head_dist = head_dist;
    }
    bool resting = p->resting;
    if (!resting && p->still_since && in.now_us - p->still_since >= MOVE_PREDICT_REST_US) {
        resting = true;
        p->rest_reason = "still";
    } else if (!resting && p->quiet_since && head_dist - p->quiet_head_dist >= MOVE_PREDICT_QUIET_AWAY_M) {
        resting = true;
        p->rest_reason = "quiet, headset moved away";
    } else if (resting && p->moving_since && in.now_us - p->moving_since >= MOVE_PREDICT_MOVE_US) {
        resting = false;
    }
    if (resting != p->resting) {
        p->resting = resting;
        p->rest_changed = true;
    }
}

// Starting point of static_error: the body and the controller's direction now.
static void set_reference(MovePredict *p, const MovePredictInput &in, Vec3 pos)
{
    p->ref_head = in.head;
    p->ref_body_yaw = p->body_yaw;
    p->ref_forward = rotate(in.orientation, v3(0, 0, -1));
    const float dx = pos.x - in.head.x, dz = pos.z - in.head.z;
    p->ref_lever = sqrtf(dx * dx + dz * dz);
}

static float static_error(const MovePredict *p, const MovePredictInput &in)
{
    const Vec3 f = rotate(in.orientation, v3(0, 0, -1)), r = p->ref_forward;
    float c = f.x * r.x + f.y * r.y + f.z * r.z;
    c = c > 1.0f ? 1.0f : c < -1.0f ? -1.0f : c;
    return len3(in.head - p->ref_head) + fabsf(wrap_pi(p->body_yaw - p->ref_body_yaw)) * p->ref_lever +
           acosf(c) * MOVE_PREDICT_FOREARM_M;
}

void move_predict_update(MovePredict *p, const MovePredictInput &in)
{
    p->changed_from = -1;
    float dt = p->last_us && in.now_us > p->last_us ? (in.now_us - p->last_us) * 1e-6f : 0.0f;
    if (dt > 0.1f)
        dt = 0.1f;
    p->last_us = in.now_us;
    update_body(p, in);
    update_rest(p, in, dt);
    p->velocity = in.velocity;

    if (!in.has_position) {
        set_mode(p, MOVE_PREDICT_NONE);
        p->offset = v3(0, 0, 0);
        p->out = in.position;
        return;
    }

    if (in.seen) {
        if (p->mode == MOVE_PREDICT_INERTIAL || p->mode == MOVE_PREDICT_ARM)
            p->offset = p->out - in.position; // glide from where it was shown
        else if (p->mode != MOVE_PREDICT_TRACKED)
            p->offset = v3(0, 0, 0); // from searching: straight to it
        set_mode(p, MOVE_PREDICT_TRACKED);
        float speed = len3(in.velocity);
        speed = speed > GLIDE_SPEED_MAX ? GLIDE_SPEED_MAX : speed;
        decay(&p->offset, dt, GLIDE_TAU_S / (1.0f + speed / GLIDE_SPEED_REF));
        p->out = in.position + p->offset;
        p->seen_pos = in.position;
        p->seen_rot = in.orientation;
        p->seen_head = in.head;
        p->seen_body_yaw = p->body_yaw;
        return;
    }

    // Not seen.
    if (p->mode == MOVE_PREDICT_TRACKED || p->mode == MOVE_PREDICT_NONE) {
        p->lost_us = in.now_us;
        p->lost_pos = p->mode == MOVE_PREDICT_TRACKED ? p->out : in.position;
        p->lost_tracker_pos = in.position;
        p->lost_head_dist = in.head_valid ? len3(p->lost_pos - in.head) : 1e9f;
        p->near = in.head_valid && p->body_valid && p->mode == MOVE_PREDICT_TRACKED &&
                  p->lost_head_dist <= MOVE_PREDICT_REACH_M;
        set_reference(p, in, p->lost_pos);
        set_mode(p, MOVE_PREDICT_INERTIAL);
    }
    if (p->mode == MOVE_PREDICT_ARM && (p->resting || !(in.worn && in.head_valid))) {
        // Put down, or the headset taken off: back to the tracker's estimate (where the
        // IMU last placed it), gliding there.
        p->offset = p->out - in.position;
        set_mode(p, MOVE_PREDICT_INERTIAL);
        if (!in.worn || !in.head_valid)
            p->near = false;
    }
    if (p->rest_changed && !p->resting && p->mode == MOVE_PREDICT_INERTIAL) {
        // Picked up again: what follows (searching included) is measured from here.
        p->lost_us = in.now_us;
        p->lost_tracker_pos = in.position;
        p->seen_pos = p->out;
        p->seen_rot = in.orientation;
        p->seen_head = in.head;
        p->seen_body_yaw = p->body_yaw;
        p->near = in.head_valid && p->body_valid && len3(p->out - in.head) <= MOVE_PREDICT_REACH_M;
        set_reference(p, in, p->out);
    }
    const uint64_t lost_for = in.now_us - p->lost_us;
    const float drift = len3(in.position - p->lost_tracker_pos); // the tracker's estimate

    // How far the tracker's estimate is likely off: a hand-held controller goes along with
    // the body (head moved, body turned around the head) and with its own rotation (the
    // sphere swings around the wrist and elbow). The arm model takes over only once that
    // is more than its own typical error.
    p->static_error = static_error(p, in);
    if (p->mode == MOVE_PREDICT_INERTIAL && p->near && in.worn && in.head_valid && !p->resting &&
        lost_for >= MOVE_PREDICT_ARM_MIN_US && p->static_error >= MOVE_PREDICT_ARM_SWITCH_M) {
        // Elbow behind the reference sphere along the controller, relative to the head and
        // the body's yaw then.
        const Vec3 elbow = p->seen_pos - forearm(p->seen_rot);
        p->elbow_body = yaw_rotate(-p->seen_body_yaw, elbow - p->seen_head);
        const Vec3 model = arm_position(p, in);
        p->offset = p->out - model; // no jump: blends from the estimate shown
        set_mode(p, MOVE_PREDICT_ARM);
    }

    if (p->mode == MOVE_PREDICT_ARM) {
        decay(&p->offset, dt, ARM_BLEND_TAU_S);
        p->out = arm_position(p, in) + p->offset;
        p->velocity = v3(0, 0, 0);
        return;
    }

    // Searching only for a controller in hand (one put down stays where it lies).
    if (p->mode == MOVE_PREDICT_INERTIAL && !p->resting && lost_for >= MOVE_PREDICT_SEARCH_US &&
        drift > MOVE_PREDICT_SEARCH_DRIFT_M) {
        set_mode(p, MOVE_PREDICT_SEARCHING);
        p->offset = v3(0, 0, 0);
    }
    if (p->mode == MOVE_PREDICT_SEARCHING) {
        p->out = in.position;
        return;
    }
    // The tracker's estimate (a glide in progress carries on).
    decay(&p->offset, dt, GLIDE_TAU_S);
    p->out = in.position + p->offset;
}

const char *move_predict_mode_name(int mode)
{
    switch (mode) {
    case MOVE_PREDICT_NONE: return "never seen";
    case MOVE_PREDICT_TRACKED: return "tracked";
    case MOVE_PREDICT_INERTIAL: return "lost (tracker estimate)";
    case MOVE_PREDICT_ARM: return "lost (arm model)";
    case MOVE_PREDICT_SEARCHING: return "searching";
    default: return "?";
    }
}
