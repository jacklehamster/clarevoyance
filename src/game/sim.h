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
#include <map>
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

// A conversation in progress (see dialogue.h). Inactive between conversations.
struct DialogueRun {
    bool        active = false;
    std::string id;                 // which dialogue
    int         line = -1;          // index of the line on screen
    uint64_t    lineTick = 0;       // tick that line came up (drives the typewriter)
    int         shown = 0;          // glyphs revealed so far
    bool        revealed = false;   // the whole line is out
    bool        footBuilt = false;  // choices / "more" marker are on screen
    int         choice = 0;         // highlighted choice
    EntityId    partner = 0;        // who we are talking to (0 = nobody: narration)
    Vec4        partnerAnim = {0, 1, 0, 0};  // their animation, to restore afterwards
};

// What the screen overlay currently shows. Overlay instances live in the
// renderer only; the sim just remembers their ids, by group, to replace them.
struct UiRun {
    std::map<std::string, std::vector<EntityId>> groups;
    EntityId hintTarget = 0;        // entity the "talk to" hint is about (0 = none)
    bool     inventoryDirty = false;
    uint64_t toastUntil = 0;        // tick the current toast disappears
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
    // What the player carries: item id → count. Ordered, so the HUD lists it
    // the same way every time.
    std::map<std::string, int> inventory;
    DialogueRun talk;
    UiRun ui;

    // The follow camera's facing and view. Sim state, not render state: its
    // facing decides which way "forward" is for camera-relative movement.
    RigState rig;

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

// Where a patrol is at `tick`: the start point, velocity and time origin of the
// leg it is currently walking. A pure function of the tick counter.
void patrolPose(const Patrol& patrol, uint64_t tick,
                Vec3& pos, Vec3& vel, float& motionStart);

// Queue an entity's current instance for the renderer, and bring everything
// attached to it (its label glyphs) along: they take the same motion formula.
// Call after changing an entity's pos/vel/accel/motionStart.
void emitEntity(const Scene& scene, SimState& state, EntityId id, Diff& out);

// Remove an entity and everything attached to it from the sim and the renderer.
void removeEntity(const Scene& scene, SimState& state, EntityId id, Diff& out);

} // namespace cv
