// sim.cpp — fixed-tick simulation step over an immutable Scene + mutable SimState.
#include "sim.h"
#include "dialogue.h"
#include "events.h"

#include <cmath>

namespace cv {

namespace {

// Abstract action names the sim itself understands. A scene's "controls"
// block binds keys to these.
//
// Movement: with a camera rig the four directions are relative to the way the
// camera faces (north = away from the camera); without one they are the world
// axes (north = -Z, east = +X).
const char* ACT_NORTH = "move_north";
const char* ACT_SOUTH = "move_south";
const char* ACT_WEST  = "move_west";
const char* ACT_EAST  = "move_east";
// Camera rig: quarter turns and the top-down / dungeon view switch.
const char* ACT_CAM_LEFT  = "camera_left";
const char* ACT_CAM_RIGHT = "camera_right";
const char* ACT_CAM_VIEW  = "camera_view";

// A desired velocity counts as "the same" as the current one when every component
// is within this tolerance — avoids emitting upserts for floating-point noise.
const float VEL_EPSILON = 1e-4f;

// An entity already touching an obstacle (by rounding error) is tested with
// its radius shrunk by this much, so it can still slide along the obstacle
// instead of sticking to it. It is also the most it can ever sink in.
const float COLLISION_SKIN = 0.01f;

// A direction of travel must have at least this much sideways component (in
// camera space) to turn the sprite around — walking straight toward or away
// from the camera keeps the current facing.
const float FACE_EPSILON = 1e-3f;

// The rig's screen-right axis, or world east when the scene has no rig.
Vec3 screenRight(const Scene& scene, const SimState& state) {
    if (!scene.rig.enabled) return {1, 0, 0};
    Vec3 forward, right;
    rigAxes(state.rig, forward, right);
    return right;
}

// Mirror the sprite so it faces its direction of travel (the art faces right).
// Returns true if the instance changed.
bool applyFacing(Instance& inst, const EntityAttrs& attrs, Vec3 right) {
    float side = dot(attrs.faceDir, right);
    if (std::fabs(side) < FACE_EPSILON) return false;
    float width = std::fabs(inst.scale.x);
    float want = side < 0.0f ? -width : width;
    if (want == inst.scale.x) return false;
    inst.scale.x = want;
    return true;
}

// Is a circle of radius r at (x, z) inside something solid — terrain, or
// another entity that blocks the way?
bool blocked(const Scene& scene, const SimState& state,
             const std::unordered_map<EntityId, Vec3>& positions,
             EntityId self, float x, float z, float r) {
    if (scene.terrain.blocksCircle(x, z, r)) return true;
    for (EntityId other : scene.solidIds) {
        if (other == self) continue;
        auto p = positions.find(other);
        auto a = state.attrs.find(other);
        if (p == positions.end() || a == state.attrs.end()) continue;  // removed
        float dx = x - p->second.x, dz = z - p->second.z;
        float reach = r + a->second.radius;
        if (dx * dx + dz * dz < reach * reach) return true;
    }
    return false;
}

void setClip(const Scene& scene, EntityId id, Instance& inst, const char* name, float now) {
    if (const Clip* clip = scene.clipOf(id, name))
        setAnimation(inst, clip->first, clip->count, clip->fps, now);
}

// Camera rig input: quarter turns and the view switch, on key press.
void stepRig(const Scene& scene, SimState& state, const ResolvedActions& actions,
             double now, Diff& out) {
    if (!scene.rig.enabled) return;
    int turn = 0;
    if (actions.pressed.count(ACT_CAM_LEFT))  turn -= 1;
    if (actions.pressed.count(ACT_CAM_RIGHT)) turn += 1;
    if (actions.pressed.count(ACT_CAM_VIEW))
        rigSetView(state.rig, state.rig.view == RIG_TOPDOWN ? RIG_DUNGEON : RIG_TOPDOWN, now);
    if (turn == 0) return;

    rigTurn(state.rig, turn, now);
    // "Screen right" just changed, so every sprite that faces its direction of
    // travel may need mirroring.
    const Vec3 right = screenRight(scene, state);
    for (auto& kv : state.attrs) {
        if (!kv.second.autoAnim) continue;
        auto it = state.entities.find(kv.first);
        if (it != state.entities.end() && applyFacing(it->second, kv.second, right))
            out.upserts.emplace_back(kv.first, it->second);
    }
}

// Patrols: start a new leg on the tick the previous one ends. On the very
// first step every patrol is re-synced to the current tick, whatever it is.
void stepPatrols(const Scene& scene, SimState& state, bool firstStep, Diff& out) {
    const uint64_t tick = state.clock.tick;
    const Vec3 right = screenRight(scene, state);
    for (EntityId id : scene.patrolIds) {
        auto it = state.entities.find(id);
        auto at = state.attrs.find(id);
        if (it == state.entities.end() || at == state.attrs.end()) continue;
        const Patrol& patrol = at->second.patrol;
        if (!firstStep && (tick + patrol.phaseTicks) % patrol.legTicks != 0) continue;

        Instance& inst = it->second;
        Vec3 vel;
        patrolPose(patrol, tick, inst.pos, vel, inst.motionStart);
        inst.vel = vel;
        at->second.faceDir = vel;
        if (at->second.autoAnim) applyFacing(inst, at->second, right);
        emitEntity(scene, state, id, out);
    }
}

// Free movement of every controlled entity, stopped by walls and solid things.
void stepMovement(const Scene& scene, SimState& state, const ResolvedActions& actions,
                  float now, const std::unordered_map<EntityId, Vec3>& positions,
                  Diff& out) {
    // Sum the held directions into a unit direction in the ground plane.
    Vec3 forward = {0, 0, -1}, right = {1, 0, 0};
    if (scene.rig.enabled) rigAxes(state.rig, forward, right);
    float ahead = 0.0f, side = 0.0f;
    if (actions.isDown(ACT_NORTH)) ahead += 1.0f;
    if (actions.isDown(ACT_SOUTH)) ahead -= 1.0f;
    if (actions.isDown(ACT_EAST))  side  += 1.0f;
    if (actions.isDown(ACT_WEST))  side  -= 1.0f;
    Vec3 dir = forward * ahead + right * side;
    if (dot(dir, dir) > 0.0f) dir = normalize(dir);

    const bool collide = scene.terrain.enabled || !scene.solidIds.empty();
    const float dt = static_cast<float>(SIM_DT);

    for (auto& kv : state.entities) {
        EntityId id = kv.first;
        auto a = state.attrs.find(id);
        if (a == state.attrs.end() || !a->second.controlled) continue;
        EntityAttrs& attrs = a->second;

        Vec3 desired = dir * attrs.speed;
        auto p = positions.find(id);

        // Collision: try the step one axis at a time and cancel the axis that
        // would end inside something. Blocking one axis and not the other is
        // what lets the entity slide along a wall.
        if (collide && p != positions.end() && dot(desired, desired) > 0.0f) {
            const Vec3 at = p->second;
            float r = attrs.radius;
            if (blocked(scene, state, positions, id, at.x, at.z, r)) r -= COLLISION_SKIN;
            if (desired.x != 0.0f &&
                blocked(scene, state, positions, id, at.x + desired.x * dt, at.z, r))
                desired.x = 0.0f;
            if (desired.z != 0.0f &&
                blocked(scene, state, positions, id,
                        at.x + desired.x * dt, at.z + desired.z * dt, r))
                desired.z = 0.0f;
        }

        Instance& inst = kv.second;
        Vec3 d = desired - inst.vel;
        if (dot(d, d) <= VEL_EPSILON * VEL_EPSILON)
            continue;  // velocity unchanged — skip the upsert (no per-frame churn)

        const bool wasMoving = dot(inst.vel, inst.vel) > 0.0f;
        const bool isMoving  = dot(desired, desired) > 0.0f;

        // Rebase the motion origin to the entity's current position so it
        // continues from where it is rather than snapping back to pos.
        if (p != positions.end()) inst.pos = p->second;
        setMotion(inst, desired, {0, 0, 0}, now);

        if (attrs.autoAnim) {
            if (isMoving != wasMoving)
                setClip(scene, id, inst, isMoving ? "walk" : "idle", now);
            if (isMoving) {
                attrs.faceDir = desired;
                applyFacing(inst, attrs, right);
            }
        }
        emitEntity(scene, state, id, out);
    }
}

} // namespace

void patrolPose(const Patrol& patrol, uint64_t tick,
                Vec3& pos, Vec3& vel, float& motionStart) {
    const uint64_t along  = tick + patrol.phaseTicks;
    const uint64_t leg    = along / patrol.legTicks;
    const uint64_t within = along % patrol.legTicks;
    const bool outbound = (leg % 2) == 0;
    const Vec3 from = outbound ? patrol.a : patrol.b;
    const Vec3 to   = outbound ? patrol.b : patrol.a;
    const float legSeconds = static_cast<float>(patrol.legTicks * SIM_DT);
    pos = from;
    vel = (to - from) * (1.0f / legSeconds);
    // The leg began `within` ticks ago — possibly before tick 0.
    motionStart = static_cast<float>(
        (static_cast<double>(tick) - static_cast<double>(within)) * SIM_DT);
}

void emitEntity(const Scene& scene, SimState& state, EntityId id, Diff& out) {
    auto it = state.entities.find(id);
    if (it == state.entities.end()) return;
    const Instance& inst = it->second;
    out.upserts.emplace_back(id, inst);

    auto lab = scene.labels.find(id);
    if (lab == scene.labels.end()) return;
    auto at = state.attrs.find(id);
    const float dy = at != state.attrs.end() ? at->second.labelDy : 0.0f;
    for (EntityId gid : lab->second) {
        auto g = state.entities.find(gid);
        if (g == state.entities.end()) continue;
        g->second.pos = inst.pos + Vec3{0.0f, dy, 0.0f};
        setMotion(g->second, inst.vel, inst.accel, inst.motionStart);
        out.upserts.emplace_back(gid, g->second);
    }
}

void removeEntity(const Scene& scene, SimState& state, EntityId id, Diff& out) {
    state.entities.erase(id);
    state.attrs.erase(id);
    out.removals.push_back(id);
    auto lab = scene.labels.find(id);
    if (lab == scene.labels.end()) return;
    for (EntityId gid : lab->second) {
        if (state.entities.erase(gid)) out.removals.push_back(gid);
    }
}

SimState makeSimState(const Scene& scene) {
    SimState state;
    state.entities = scene.initialState.instances;
    state.attrs    = scene.attrs;
    state.fired.assign(scene.events.size(), 0);
    state.rig      = makeRigState(scene.rig);
    return state;
}

void stepSim(const Scene& scene, SimState& state,
             const ResolvedActions& actions, Diff& out) {
    const double simTime = state.clock.time();
    const float  now     = static_cast<float>(simTime);
    const bool firstStep = !state.started;

    // A conversation takes over the controls: its keys drive the dialogue, and
    // nothing reaches the camera or the player's legs (the world keeps going).
    const bool talking = state.talk.active;
    const ResolvedActions none;
    const ResolvedActions& worldActions = talking ? none : actions;

    stepRig(scene, state, worldActions, simTime, out);
    stepPatrols(scene, state, firstStep, out);

    // Entity positions at this tick, from the analytic motion model.
    std::unordered_map<EntityId, Vec3> positions;
    positions.reserve(state.entities.size());
    for (const auto& kv : state.entities)
        positions[kv.first] = motionAt(kv.second, now);

    stepMovement(scene, state, worldActions, now, positions, out);

    if (talking) stepDialogue(scene, state, actions, out);
    else         stepInteractions(scene, state, actions, positions, out);

    // Events (including input triggers) run against the resolved actions.
    // updateEvents mutates state (flags, fired, entities, attrs) directly and
    // appends the renderer-facing upserts/removals to `out`.
    updateEvents(scene, state, simTime, positions, worldActions, out);

    refreshHud(scene, state, positions, out);

    state.clock.tick++;

    // Expire runtime-spawned dialogue text (Scene::dialogueText — see
    // Action::Type::Dialogue in events.cpp). Tick-driven, so replaying the
    // same input sequence always expires text on the same tick.
    for (auto it = state.dialogueTextExpiry.begin();
         it != state.dialogueTextExpiry.end(); ) {
        if (state.clock.tick >= it->first) {
            for (EntityId id : it->second) {
                state.entities.erase(id);
                out.removals.push_back(id);
            }
            it = state.dialogueTextExpiry.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace cv
