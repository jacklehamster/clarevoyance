// camera_rig.h — the follow camera: two views, quarter-turn rotation.
//
// The rig keeps a focus point (the hero) on screen from one of two views and
// blends smoothly between them:
//
//   top-down — high and behind, looking down at about 45°: the overworld view;
//   dungeon  — low and close behind the hero, looking ahead down the corridor:
//              a dungeon crawler where you still see your character.
//
// Its yaw is locked to 90° steps (a hard design constraint): the rig only ever
// rests facing north, east, south or west, and turning is a short eased sweep
// between two of those — never a snap, never a diagonal.
//
// Everything here is pure math over plain data, no GL. RigState is part of the
// simulation state (the rig's facing decides which way "forward" is for
// movement, so it must replay identically); rigCamera() turns that state into
// a Camera for any continuous render time.
#pragma once

#include <cmath>

#include "camera.h"

namespace cv {

constexpr float RIG_HALF_PI = 1.5707963f;

// How long a quarter turn and a view switch take, in seconds.
constexpr float RIG_TURN_SECONDS = 0.35f;
constexpr float RIG_VIEW_SECONDS = 0.70f;

// The camera never closes in on its pivot beyond this fraction of the full
// distance, however tight the corridor (it would end up inside the hero).
constexpr float RIG_MIN_PULL = 0.12f;

// As a wall pushes the camera in, it also rises by up to this much, so a hero
// backed against a wall is seen from above the head rather than from inside it.
constexpr float RIG_PULL_LIFT = 0.95f;

// While a dialogue window covers the bottom of the screen, the dungeon view
// aims this much lower, which lifts the hero clear of the window.
constexpr float RIG_TALK_DROP = 0.6f;

// One view = where the eye sits relative to the focus, and what it looks at.
struct RigView {
    float back = 7.0f;          // horizontal distance behind the focus
    float height = 7.0f;        // eye height above the ground
    float lookAhead = 0.0f;     // aim this far in front of the focus
    float targetHeight = 0.5f;  // aim height above the ground
    float fovY = 0.8727f;       // vertical field of view, radians (50°)
};

enum RigViewId { RIG_TOPDOWN = 0, RIG_DUNGEON = 1 };

struct RigConfig {
    bool enabled = false;
    RigView topdown = {7.0f, 7.0f, 0.0f, 0.5f, 0.8727f};
    RigView dungeon = {2.7f, 1.35f, 2.5f, 0.75f, 1.2217f};  // 70° fov
    int startView = RIG_TOPDOWN;
    int startQuarter = 0;       // 0 = facing north (-Z), 1 = east, 2 = south, 3 = west
};

// The rig's facing and view, plus where each was coming from, so the eased
// transition can be evaluated at any time without per-frame state.
struct RigState {
    int    quarter = 0;         // target facing in quarter turns (unbounded)
    float  yawFrom = 0.0f;      // yaw (radians) when the current turn began
    double turnTime = -1000.0;  // sim time the current turn began
    int    view = RIG_TOPDOWN;  // target view
    float  blendFrom = 0.0f;    // view blend (0 top-down .. 1 dungeon) at switch
    double viewTime = -1000.0;  // sim time the current switch began
};

inline RigState makeRigState(const RigConfig& cfg) {
    RigState s;
    s.quarter = cfg.startQuarter;
    s.yawFrom = cfg.startQuarter * RIG_HALF_PI;
    s.view = cfg.startView == RIG_DUNGEON ? RIG_DUNGEON : RIG_TOPDOWN;
    s.blendFrom = static_cast<float>(s.view);
    return s;
}

// Smoothstep: eases in and out, exact at both ends.
inline float rigEase(double elapsed, float duration) {
    if (elapsed <= 0.0) return 0.0f;
    if (elapsed >= duration) return 1.0f;
    float p = static_cast<float>(elapsed) / duration;
    return p * p * (3.0f - 2.0f * p);
}

// Yaw in radians at time t. 0 faces north (-Z); positive turns clockwise seen
// from above (toward east).
inline float rigYaw(const RigState& s, double t) {
    float to = s.quarter * RIG_HALF_PI;
    return s.yawFrom + (to - s.yawFrom) * rigEase(t - s.turnTime, RIG_TURN_SECONDS);
}

// View blend at time t: 0 = top-down, 1 = dungeon.
inline float rigBlend(const RigState& s, double t) {
    float to = static_cast<float>(s.view);
    return s.blendFrom + (to - s.blendFrom) * rigEase(t - s.viewTime, RIG_VIEW_SECONDS);
}

// Start a quarter turn (dir = +1 clockwise / -1 counter-clockwise) at time
// `now`, from wherever the rig currently points — turns chain without a jump.
inline void rigTurn(RigState& s, int dir, double now) {
    s.yawFrom = rigYaw(s, now);
    s.turnTime = now;
    s.quarter += dir;
}

inline void rigSetView(RigState& s, int view, double now) {
    s.blendFrom = rigBlend(s, now);
    s.viewTime = now;
    s.view = view;
}

// Unit ground-plane direction the rig faces at a given yaw.
inline Vec3 rigForward(float yaw) {
    return {std::sin(yaw), 0.0f, -std::cos(yaw)};
}

// The exact forward/right axes for the rig's TARGET facing — integers, so
// camera-relative movement is bit-exact however the turn is animated.
inline void rigAxes(const RigState& s, Vec3& forward, Vec3& right) {
    static const float FX[4] = {0, 1, 0, -1};
    static const float FZ[4] = {-1, 0, 1, 0};
    int q = ((s.quarter % 4) + 4) % 4;
    forward = {FX[q], 0.0f, FZ[q]};
    right   = {-FZ[q], 0.0f, FX[q]};
}

inline RigView rigMix(const RigView& a, const RigView& b, float t) {
    auto mix = [t](float x, float y) { return x + (y - x) * t; };
    return {mix(a.back, b.back), mix(a.height, b.height),
            mix(a.lookAhead, b.lookAhead), mix(a.targetHeight, b.targetHeight),
            mix(a.fovY, b.fovY)};
}

// Where the rig wants to be at time t, before any wall gets in the way:
// `pivot` is the point on the focus the eye swings around, `eye` the ideal
// camera position.
struct RigPose {
    RigView view;
    Vec3 forward;
    Vec3 pivot;
    Vec3 eye;
};

inline RigPose rigPose(const RigConfig& cfg, const RigState& s, double t, Vec3 focus) {
    RigPose p;
    p.view = rigMix(cfg.topdown, cfg.dungeon, rigBlend(s, t));
    p.forward = rigForward(rigYaw(s, t));
    p.pivot = {focus.x, focus.y + p.view.targetHeight, focus.z};
    p.eye = {focus.x - p.forward.x * p.view.back,
             focus.y + p.view.height,
             focus.z - p.forward.z * p.view.back};
    return p;
}

// Build the camera for a pose. `pull` in (0, 1] slides the eye along the line
// toward the pivot — 1 is the ideal position, smaller values bring it in front
// of a wall that would otherwise hide the hero. `drop` lowers the aim point
// (tilting the view down), in world units.
inline Camera rigCamera(const RigPose& p, float pull, float drop = 0.0f) {
    if (pull < RIG_MIN_PULL) pull = RIG_MIN_PULL;
    if (pull > 1.0f) pull = 1.0f;
    Camera cam;
    cam.projection = Projection::Perspective;
    cam.fovYRadians = p.view.fovY;
    cam.position = p.pivot + (p.eye - p.pivot) * pull;
    cam.position.y += (1.0f - pull) * RIG_PULL_LIFT;
    cam.target = {p.pivot.x + p.forward.x * p.view.lookAhead,
                  p.pivot.y - drop,
                  p.pivot.z + p.forward.z * p.view.lookAhead};
    return cam;
}

} // namespace cv
