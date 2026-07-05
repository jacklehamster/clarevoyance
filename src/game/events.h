// events.h — the data-driven event/condition/action system.
//
// Game logic (dialogue, flag changes, animation swaps) is expressed as data:
// each Event is a Trigger + an optional Condition + a list of Actions (see
// scene.h for the data structs). updateEvents evaluates them once per
// simulation tick against the immutable Scene and the mutable SimState, and
// emits a Diff for the renderer plus side effects (flags, dialogue). It never
// calls GL.
//
// Determinism: triggers are pure functions of sim state (entity positions and
// flags). Same state in → same events fire. No wall-clock, no rand().
#pragma once

#include <unordered_map>

#include "world_state.h"
#include "input.h"
#include "scene.h"
#include "sim.h"

namespace cv {

// Evaluate all scene events against the given entity positions (keyed by id)
// at sim time `now`, with the abstract input actions resolved for this tick.
// Mutates `state` (flags, fired markers, entities, attrs), appends renderer
// upserts/removals to `out`, and applies dialogue side effects. Start triggers
// fire on the first step only (state.started). Normally called via stepSim().
void updateEvents(const Scene& scene,
                  SimState& state,
                  double now,
                  const std::unordered_map<EntityId, Vec3>& positions,
                  const ResolvedActions& actions,
                  Diff& out);

} // namespace cv
