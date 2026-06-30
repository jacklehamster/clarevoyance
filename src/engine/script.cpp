#include "script.h"

#include <algorithm>
#include <cmath>

namespace cv {

// ---------------------------------------------------------------------------
// Analytic exit time
// ---------------------------------------------------------------------------

float ScriptRunner::computeExitTime(const Instance& inst, const ExitCondition& exit,
                                     float activationTime) const {
    if (exit.type == ExitCondition::Type::Time) {
        return activationTime + exit.value;
    }

    // GroundContact: solve pos.y + vel.y*dt + 0.5*accel.y*dt^2 = groundY
    // Rearranged:    0.5*a*dt^2 + v*dt + y0 = 0,  y0 = pos.y - groundY
    const float eps  = 1e-4f;
    const float inf  = 1e30f;
    float y0 = inst.pos.y - exit.value;
    float v  = inst.vel.y;
    float a  = inst.accel.y;

    if (std::abs(a) < eps) {
        // Linear case
        if (std::abs(v) < eps) return inf; // stationary, never hits
        float dt = -y0 / v;
        return (dt > eps) ? inst.motionStart + dt : inf;
    }

    // Quadratic case
    float disc = v * v - 2.0f * a * y0;
    if (disc < 0.0f) return inf; // no real roots
    float sqrtDisc = std::sqrt(disc);
    float dt1 = (-v + sqrtDisc) / a;
    float dt2 = (-v - sqrtDisc) / a;

    // Take smallest positive root beyond a tiny epsilon (skip the launch point).
    float result = inf;
    if (dt1 > eps && dt1 < result) result = dt1;
    if (dt2 > eps && dt2 < result) result = dt2;
    return (result < inf) ? inst.motionStart + result : inf;
}

// ---------------------------------------------------------------------------
// Phase activation
// ---------------------------------------------------------------------------

void ScriptRunner::activatePhase(ActiveEntry& e, int phaseIdx,
                                  float activationTime, Diff& diff) {
    e.phase = phaseIdx;
    Instance inst      = e.script.phases[phaseIdx].inst;
    inst.motionStart   = activationTime;
    e.current          = inst;
    e.exitTime         = computeExitTime(inst, e.script.phases[phaseIdx].exit,
                                         activationTime);
    diff.upserts.push_back({e.script.id, inst});
}

// ---------------------------------------------------------------------------
// Repeat step
// ---------------------------------------------------------------------------

void ScriptRunner::stepRepeat(ActiveEntry& e, float transitionTime, Diff& diff) {
    Instance next = e.script.repeat.transition(e.current, transitionTime);
    if (e.script.repeat.stop(next)) {
        // Bring the entity to rest at ground level.
        next.vel   = {0.0f, 0.0f, 0.0f};
        next.accel = {0.0f, 0.0f, 0.0f};
        e.current  = next;
        e.done     = true;
        diff.upserts.push_back({e.script.id, next});
        return;
    }
    e.current  = next;
    e.exitTime = computeExitTime(next, e.script.repeat.exit, transitionTime);
    diff.upserts.push_back({e.script.id, next});
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void ScriptRunner::add(EntityScript script, float startTime, Diff& diff) {
    ActiveEntry e;
    e.script = std::move(script);
    e.done   = false;

    if (e.script.hasRepeat) {
        e.current              = e.script.repeat.seed;
        e.current.motionStart  = startTime;
        e.exitTime             = computeExitTime(e.current, e.script.repeat.exit,
                                                  startTime);
        diff.upserts.push_back({e.script.id, e.current});
    } else if (!e.script.phases.empty()) {
        activatePhase(e, 0, startTime, diff);
    }

    active_.push_back(std::move(e));
}

void ScriptRunner::remove(EntityId id) {
    active_.erase(std::remove_if(active_.begin(), active_.end(),
                                  [id](const ActiveEntry& e) {
                                      return e.script.id == id;
                                  }),
                  active_.end());
}

void ScriptRunner::replace(EntityScript script, float startTime, Diff& diff) {
    remove(script.id);
    add(std::move(script), startTime, diff);
}

void ScriptRunner::advance(float simTime, Diff& diff) {
    for (auto& e : active_) {
        if (e.done) continue;

        // Drain all transitions that fired at or before simTime.
        // Multiple can fire in one tick if phases are very short.
        while (e.exitTime <= simTime && !e.done) {
            float transitionTime = e.exitTime;

            if (e.script.hasRepeat) {
                stepRepeat(e, transitionTime, diff);
            } else {
                int next = e.phase + 1;
                if (next >= static_cast<int>(e.script.phases.size())) {
                    if (e.script.loop) next = 0;
                    else { e.done = true; break; }
                }
                activatePhase(e, next, transitionTime, diff);
            }
        }
    }

    // Purge finished scripts.
    active_.erase(std::remove_if(active_.begin(), active_.end(),
                                  [](const ActiveEntry& e) { return e.done; }),
                  active_.end());
}

} // namespace cv
