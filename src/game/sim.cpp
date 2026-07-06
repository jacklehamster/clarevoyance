// sim.cpp — fixed-tick simulation step over an immutable Scene + mutable SimState.
#include "sim.h"
#include "events.h"

namespace cv {

// CPU mirror of the GPU motion formula:
//   pos(t) = pos + vel*(t-start) + 0.5*accel*(t-start)^2
static Vec3 positionAt(const Instance& inst, float t) {
    float dt = t - inst.motionStart;
    return inst.pos + inst.vel * dt + inst.accel * (0.5f * dt * dt);
}

SimState makeSimState(const Scene& scene) {
    SimState state;
    state.entities = scene.initialState.instances;
    state.attrs    = scene.attrs;
    state.fired.assign(scene.events.size(), 0);
    return state;
}

void stepSim(const Scene& scene, SimState& state,
             const ResolvedActions& actions, Diff& out) {
    const double simTime = state.clock.time();
    const float  now     = static_cast<float>(simTime);

    // Entity positions at this tick, from the analytic motion model.
    std::unordered_map<EntityId, Vec3> positions;
    positions.reserve(state.entities.size());
    for (const auto& kv : state.entities)
        positions[kv.first] = positionAt(kv.second, now);

    // Free movement: applies a velocity to every controlled entity.
    applyMovement(actions, state.attrs, state.entities, now, positions, out);

    // Events (including input triggers) run against the resolved actions.
    // updateEvents mutates state (flags, fired, entities, attrs) directly and
    // appends the renderer-facing upserts/removals to `out`.
    updateEvents(scene, state, simTime, positions, actions, out);

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
