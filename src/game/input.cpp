// input.cpp — InputFrame → abstract actions → free movement.
#include "input.h"

namespace cv {

// Abstract action names recognised by the movement system. Bindings map keys to
// these; the scene's "controls" block is free to bind any key(s) to each.
static const char* ACT_NORTH = "move_north";  // -Z
static const char* ACT_SOUTH = "move_south";  // +Z
static const char* ACT_WEST  = "move_west";   // -X
static const char* ACT_EAST  = "move_east";   // +X

// A desired velocity counts as "the same" as the current one when every component
// is within this tolerance — avoids emitting upserts for floating-point noise.
static const float VEL_EPSILON = 1e-4f;

ResolvedActions resolveActions(const InputFrame& in, const Bindings& bindings) {
    ResolvedActions out;
    auto map = [&](const std::unordered_set<std::string>& keys,
                   std::unordered_set<std::string>& dst) {
        for (const std::string& key : keys) {
            auto it = bindings.find(key);
            if (it != bindings.end()) dst.insert(it->second);
        }
    };
    map(in.down,     out.down);
    map(in.pressed,  out.pressed);
    map(in.released, out.released);
    return out;
}

void applyMovement(const ResolvedActions& actions,
                   const std::unordered_map<EntityId, EntityAttrs>& attrs,
                   std::unordered_map<EntityId, Instance>& entities,
                   float now,
                   const std::unordered_map<EntityId, Vec3>& positions,
                   Diff& out) {
    // Sum held directional actions into a world-space unit direction.
    Vec3 dir = {0, 0, 0};
    if (actions.isDown(ACT_NORTH)) dir.z -= 1.0f;
    if (actions.isDown(ACT_SOUTH)) dir.z += 1.0f;
    if (actions.isDown(ACT_WEST))  dir.x -= 1.0f;
    if (actions.isDown(ACT_EAST))  dir.x += 1.0f;
    if (dot(dir, dir) > 0.0f) dir = normalize(dir);

    // Apply the resulting velocity to every controlled entity.
    for (auto& kv : entities) {
        EntityId id = kv.first;
        auto a = attrs.find(id);
        if (a == attrs.end() || !a->second.controlled) continue;

        Vec3 desired = dir * a->second.speed;

        Instance& inst = kv.second;
        Vec3 d = desired - inst.vel;
        if (dot(d, d) <= VEL_EPSILON * VEL_EPSILON)
            continue;  // velocity unchanged — skip the upsert (no per-frame churn)

        // Rebase the motion origin to the entity's current position so it
        // continues from where it is rather than snapping back to pos.
        auto p = positions.find(id);
        if (p != positions.end()) inst.pos = p->second;
        setMotion(inst, desired, {0, 0, 0}, now);
        out.upserts.emplace_back(id, inst);
    }
}

} // namespace cv
