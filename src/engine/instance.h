// instance.h — one renderable sprite.
//
// This struct is the CPU mirror of the per-instance GPU attribute layout. Its
// fields are laid out as tightly packed floats so the whole struct can be
// uploaded straight into the instance VBO and described with offsetof(); see
// renderer.cpp. Keep it standard-layout (plain floats, no virtuals).
//
// The important idea: an Instance encodes *how it evolves over time*, not just a
// static pose. Motion (pos/vel/accel from motionStart) and animation (anim) are
// functions of the global `time` uniform, evaluated on the GPU. So a sprite in
// flight or mid-animation needs no per-frame state update — we only push a new
// Instance when its trajectory or animation actually changes.
#pragma once

#include <type_traits>
#include "mathx.h"

namespace cv {

struct Instance {
    Vec3 pos = {0, 0, 0};        // position at motionStart
    Vec3 vel = {0, 0, 0};        // velocity (world units / sec)
    Vec3 accel = {0, 0, 0};      // acceleration (e.g. gravity for a thrown arc)
    float motionStart = 0.0f;    // time origin for the motion formula

    Vec2 scale = {1, 1};         // sprite world size
    float rotation = 0.0f;       // yaw about world-up, radians (oriented sprites)
    float billboard = 0.0f;      // 1 = always face camera, else use rotation

    // Animation: firstFrame, frameCount, framesPerSecond, animStart.
    Vec4 anim = {0, 1, 0, 0};

    // Tint: RGBA multiplier applied in the fragment shader.
    // Default (1,1,1,1) = fully opaque, unmodified colour.
    // Set alpha < 1 for translucent shims; set rgb to tint the sprite.
    Vec4 tint = {1, 1, 1, 1};
};

// Compile-time ABI guard: Instance is a binary contract with the GPU attribute
// layout in renderer.cpp. These assertions catch accidental struct drift early.
static_assert(std::is_standard_layout<Instance>::value, "Instance must be standard-layout");
static_assert(sizeof(Vec2) == 8,  "Vec2 size changed — GPU attribute layout broken");
static_assert(sizeof(Vec3) == 12, "Vec3 size changed — GPU attribute layout broken");
static_assert(sizeof(Vec4) == 16, "Vec4 size changed — GPU attribute layout broken");

// --- Convenience builders (keep call sites in the demo / game readable) ------

inline Instance makeSprite(Vec3 pos, Vec2 scale, float rotation = 0.0f) {
    Instance i;
    i.pos = pos;
    i.scale = scale;
    i.rotation = rotation;
    return i;
}

inline Instance makeBillboard(Vec3 pos, Vec2 scale) {
    Instance i;
    i.pos = pos;
    i.scale = scale;
    i.billboard = 1.0f;
    return i;
}

// firstFrame..firstFrame+frameCount-1 cycled at `fps`, starting at animStart.
inline void setAnimation(Instance& i, int firstFrame, int frameCount,
                         float fps, float animStart) {
    i.anim = {static_cast<float>(firstFrame), static_cast<float>(frameCount),
              fps, animStart};
}

// Launch a sprite along a parabola: x(t) = pos + vel*(t-start) + 0.5*accel*(t-start)^2.
inline void setMotion(Instance& i, Vec3 vel, Vec3 accel, float motionStart) {
    i.vel = vel;
    i.accel = accel;
    i.motionStart = motionStart;
}

} // namespace cv
