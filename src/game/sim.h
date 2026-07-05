// sim.h — the fixed-timestep simulation clock.
//
// The game simulation advances in fixed ticks of SIM_DT seconds (60 Hz — owner
// decision, see docs/specs/SHIM_SYSTEM.md). Sim time is DERIVED from the integer
// tick counter, never from the wall clock, so the same input sequence produces
// the same trigger firings at any frame rate on desktop, WASM, and Switch.
//
// The engine's render loop runs an accumulator over wall time and steps the sim
// zero or more whole ticks per rendered frame. Rendering stays smooth because
// the renderer receives the CONTINUOUS time `clock.time() + accumulator
// remainder` — the analytic motion model (pos + vel*t + ½·accel·t²) makes that
// exact, not an approximation.
//
// Networking note: the per-tick input is conceptually a tick-stamped command —
// "resolve actions for tick N" stays a pure function of (command, bindings), so
// a lockstep peer can replay the identical command stream deterministically.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "scene.h"

namespace cv {

// Fixed simulation timestep: 60 Hz.
constexpr double SIM_DT = 1.0 / 60.0;

// Integer tick counter; sim time is derived, never stored, so it cannot drift.
struct SimClock {
    uint64_t tick = 0;
    double time() const { return static_cast<double>(tick) * SIM_DT; }
};

// ALL runtime simulation state, grouped into one cheaply-copyable value.
// The Scene stays immutable after load; everything that mutates while the
// game runs lives here. Copying a SimState and stepping the copy forward
// (the shim lookahead fork) touches neither the Scene nor the renderer.
struct SimState {
    std::unordered_map<EntityId, Instance>    entities;  // working kinematic state
    std::unordered_map<EntityId, EntityAttrs> attrs;     // controlled/speed per entity
    // Event flags: one numeric store for both booleans (0/1) and counters.
    // A flag that was never set reads as 0.0 (i.e. false).
    std::unordered_map<std::string, double>   flags;
    std::vector<uint8_t> fired;   // per-event fired markers, indexed by event position
    SimClock clock;               // the sim's own tick clock
    bool started = false;         // start triggers fire on the first step only

    // Runtime-spawned dialogue text (Scene::dialogueText config, consumed by
    // Action::Type::Dialogue in events.cpp): ids are allocated starting well
    // above any scene-authored id so they can never collide with a
    // Scene::nameToId entry.
    EntityId nextDynamicId = 1'000'000;
    // Pending expirations: (tick at which to remove, glyph ids to remove).
    // Checked once per step in stepSim — tick-driven, so it stays deterministic.
    std::vector<std::pair<uint64_t, std::vector<EntityId>>> dialogueTextExpiry;
};

// Build the initial SimState for a freshly loaded scene: snapshot the scene's
// entity instances and attributes, clear flags, size the fired markers.
SimState makeSimState(const Scene& scene);

// Advance `state` by exactly one tick against the immutable `scene`:
// resolve movement for controlled entities, evaluate events, apply the
// resulting changes to `state`, append renderer upserts/removals to `out`,
// then increment the clock. `actions` is the tick-stamped input command for
// this tick (edges must appear on exactly one tick — the caller guarantees
// that). Pure with respect to its arguments: no globals, no wall clock, so
// the same (scene, state, actions) always produces the same result.
void stepSim(const Scene& scene, SimState& state,
             const ResolvedActions& actions, Diff& out);

} // namespace cv
