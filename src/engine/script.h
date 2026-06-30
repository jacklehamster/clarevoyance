// script.h — time-driven script runner for entity motion sequences.
//
// A script is one of two things:
//   Phase list  — a finite sequence of (Instance, ExitCondition) pairs played
//                 straight through. Optional loop flag wraps back to phase 0.
//   Repeat rule — a seed Instance + a transition function applied repeatedly
//                 until a stop predicate fires. Good for physics loops like
//                 bouncing where the number of iterations isn't known upfront.
//
// The runner evaluates exit conditions analytically where possible (quadratic
// solve for GroundContact) so it never needs per-pixel simulation.
//
// Collision with other entities is out of scope — ExitCondition only inspects
// the entity's own physics state.
#pragma once

#include <functional>
#include <vector>
#include "instance.h"
#include "world_state.h"

namespace cv {

// ---------------------------------------------------------------------------
// Exit conditions
// ---------------------------------------------------------------------------

struct ExitCondition {
    enum class Type {
        Time,          // fires after `value` seconds from phase activation
        GroundContact, // fires when pos.y reaches `value` (solved analytically)
    };
    Type  type  = Type::Time;
    float value = 0.0f;
};

inline ExitCondition exitAfter(float duration) {
    return {ExitCondition::Type::Time, duration};
}
inline ExitCondition exitOnGround(float groundY = 0.0f) {
    return {ExitCondition::Type::GroundContact, groundY};
}

// ---------------------------------------------------------------------------
// Phase list script
// ---------------------------------------------------------------------------

struct Phase {
    Instance      inst; // initial conditions for this phase; motionStart is set
                        // by the runner to the exact transition time
    ExitCondition exit;
};

// ---------------------------------------------------------------------------
// Repeat rule script
// ---------------------------------------------------------------------------

struct RepeatRule {
    Instance seed; // starting Instance; motionStart set by runner at activation

    // Called at each phase boundary. Receives the current Instance and the
    // exact transition time t; returns the next Instance (with motionStart = t).
    std::function<Instance(const Instance&, float t)> transition;

    // Checked after transition. Return true to stop (entity holds last state).
    std::function<bool(const Instance&)> stop;

    ExitCondition exit; // typically exitOnGround() for physics loops
};

// ---------------------------------------------------------------------------
// EntityScript — wraps either a phase list or a repeat rule
// ---------------------------------------------------------------------------

struct EntityScript {
    EntityId           id;
    std::vector<Phase> phases;
    bool               loop      = false; // phase list: wrap to phase 0 when done
    RepeatRule         repeat;
    bool               hasRepeat = false;
};

// ---------------------------------------------------------------------------
// ScriptRunner
// ---------------------------------------------------------------------------

class ScriptRunner {
public:
    // Register a script starting at startTime. Emits the first Instance into diff.
    void add(EntityScript script, float startTime, Diff& diff);

    void remove(EntityId id);

    // Cancel any existing script for script.id, then add the new one.
    void replace(EntityScript script, float startTime, Diff& diff);

    // Advance all active scripts to simTime, emitting Diffs for any transitions.
    // Call once per simulation tick before renderer.applyDiff().
    void advance(float simTime, Diff& diff);

    bool empty() const { return active_.empty(); }

private:
    struct ActiveEntry {
        EntityScript script;
        int          phase    = 0;
        float        exitTime = 0.0f; // absolute sim time of next transition
        Instance     current;         // last emitted Instance (drives repeat transitions)
        bool         done    = false;
    };

    // Compute absolute sim time when exit condition fires for inst activated at activationTime.
    float computeExitTime(const Instance& inst, const ExitCondition& exit,
                          float activationTime) const;

    void activatePhase(ActiveEntry& e, int phaseIdx, float activationTime, Diff& diff);
    void stepRepeat(ActiveEntry& e, float transitionTime, Diff& diff);

    std::vector<ActiveEntry> active_;
};

} // namespace cv
