#pragma once

#include <stdint.h>

#include "vrmath.h"

// PS Move position while the camera does not see it, like a Quest 2 controller out of view
// (the headset is not concerned). Orientation always comes from the tracker (IMU).
//
//   Lost, the tracker first dead-reckons the position from the IMU (Beat Saber's inertial
//   behaviour, kept for fast swings out of view), then freezes it after ~3 s; back in view
//   it jumped up to ~0.5 m (2026-10-02 recordings).
//   Lost within arm's reach of the head, with the headset worn and the controller in hand:
//   an arm model takes over (no sooner than 0.5 s) once the tracker's estimate is likely
//   off by more than the arm model's own error, judged from how much the body moved (head
//   position, body turn times the hand's distance) and the controller turned since the
//   loss. The arm model: an elbow fixed to the body (following the head's position and,
//   lazily, its yaw) and the forearm along the controller's own rotation, anchored on the
//   last pose the camera saw. Never searching.
//   Put down (its accelerometer goes still: a hand always shakes it a little; or it stays
//   quiet while the headset walks away from it): back to the tracker's
//   estimate (where the IMU last placed it), gliding there, and never searching. Picked up
//   again, the above starts over from there.
//   Otherwise (further away, or the headset not worn): the tracker's position, and
//   searching only after 10 s AND more than 80 cm from where it was lost.
//   Seen again (not searching): glides to the tracked position, faster the faster the
//   controller moves.
// Pure math (no system calls): host-tested with the Tracking Viewer's recordings.

enum MovePredictMode {
    MOVE_PREDICT_NONE = 0,  // never seen (searching)
    MOVE_PREDICT_TRACKED,   // seen by the camera (possibly gliding back)
    MOVE_PREDICT_INERTIAL,  // lost: the tracker's own estimate
    MOVE_PREDICT_ARM,       // lost near the body: arm model
    MOVE_PREDICT_SEARCHING, // lost for good (SteamVR hides it)
};

struct MovePredictInput {
    uint64_t now_us;
    bool seen;            // position quality FULL or PARTIAL
    bool has_position;    // the tracker gave a position once
    Vec3 position;        // tracker's (dead-reckoned or frozen while not seen)
    Quat orientation;
    Vec3 velocity;        // tracker's, m/s (0 when it has none)
    bool has_accel;       // the raw motion sensors: accelerometer (g, gravity included)
    Vec3 accel;
    Vec3 gyro;            // rad/s
    bool head_valid;
    Vec3 head;            // centre between the eyes
    Quat head_rot;
    bool worn;            // headset on the head (proximity sensor; true when unknown)
};

struct MovePredict {
    int mode;
    uint64_t last_us;     // previous update
    uint64_t lost_us;     // when the camera lost it
    Vec3 out;             // position to use
    Vec3 offset;          // out - source, decaying (glide)
    // At the loss: last seen pose and the body then.
    Vec3 lost_pos, lost_tracker_pos;
    bool near;            // lost within arm's reach of the head
    float lost_head_dist;
    Vec3 elbow_body;      // arm model: elbow relative to the head, in the body's yaw frame
    bool body_valid;
    float body_yaw;       // radians, lazily following the head's yaw
    // Last seen pose (anchor of the arm model).
    Vec3 seen_pos;
    Quat seen_rot;
    Vec3 seen_head;
    float seen_body_yaw;
    // Rest detection: rotation speed (rad/s, smoothed); accelerometer mean and variance
    // (smoothed); its standard deviation relative to its mean (gravity), so independent of
    // the unit; quiet since (0: moving), moving since (0: quiet).
    Quat prev_rot;
    bool prev_rot_valid;
    float rot_rate;
    Vec3 accel_mean;
    float accel_var;
    bool accel_valid;
    float accel_shake;
    uint64_t still_since, moving_since;
    // Quiet (not necessarily still) since, and the headset's distance to it then: a
    // controller in hand comes along when the headset moves away.
    uint64_t quiet_since;
    float quiet_head_dist;
    const char *rest_reason; // why it was taken as put down (for the log)
    bool resting;      // put down
    bool rest_changed; // resting changed this update (for the log)
    // Body and controller direction when lost (or picked up), and the error the tracker's
    // estimate likely has since then (m).
    Vec3 ref_head;
    float ref_body_yaw, ref_lever;
    Vec3 ref_forward;
    float static_error;
    // Velocity to send (m/s): the tracker's, or 0 under the arm model.
    Vec3 velocity;
    // A transition happened this update (for the log): MovePredictMode before it, else -1.
    int changed_from;
};

// Distances and times (tuned on the 2026-10-02 recordings; see move_predict.cpp).
#define MOVE_PREDICT_REACH_M 1.00f        // "near the body": within this of the head (a hand
                                          // hanging down is ~0.85 m below the eyes; seated or
                                          // lying, the recordings showed up to 1.0 m)
#define MOVE_PREDICT_ARM_MIN_US 500000ull  // the arm model takes over no sooner than 0.5 s ...
#define MOVE_PREDICT_ARM_SWITCH_M 0.20f    // ... once the estimate is likely off by this (the
                                          // arm model was 12-32 cm off in the recordings)
// Put down: accelerometer shake (standard deviation over ~0.3 s, of gravity) under
// REST_SHAKE for REST_US; or quiet (shake under QUIET_SHAKE, gyroscope under QUIET_GYRO)
// while the headset gets QUIET_AWAY_M further from it. Picked up: shake over MOVE_SHAKE or
// gyroscope over MOVE_GYRO, for MOVE_US. From the 2026-10-08 Dev logs: lying, the shake
// is 0.0034-0.0045 (sensor noise) and the gyroscope under 0.05 rad/s; in hand, the shake
// is mostly over 0.008. The tracker's orientation of a lying controller out of view drifts
// by up to 5 degrees/s, so it is not used.
#define MOVE_PREDICT_REST_SHAKE 0.0055f
#define MOVE_PREDICT_REST_US 500000ull
#define MOVE_PREDICT_QUIET_SHAKE 0.012f
#define MOVE_PREDICT_QUIET_GYRO 0.15f
#define MOVE_PREDICT_QUIET_AWAY_M 0.30f
#define MOVE_PREDICT_MOVE_SHAKE 0.015f
#define MOVE_PREDICT_MOVE_GYRO 0.30f
#define MOVE_PREDICT_MOVE_US 150000ull
#define MOVE_PREDICT_SEARCH_US 10000000ull // searching after 10 s ...
#define MOVE_PREDICT_SEARCH_DRIFT_M 0.80f  // ... and this far from where it was lost
#define MOVE_PREDICT_FOREARM_M 0.30f       // elbow to the sphere
#define MOVE_PREDICT_BODY_YAW_RAD 0.61f    // body yaw follows the head beyond 35 degrees

void move_predict_reset(MovePredict *p);
void move_predict_update(MovePredict *p, const MovePredictInput &in);
const char *move_predict_mode_name(int mode);
