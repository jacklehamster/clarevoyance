// events.h — the data-driven event/condition/action system.
//
// Game logic (dialogue, flag changes, animation swaps) is expressed as data:
// each Event is a Trigger + an optional Condition + a list of Actions (see
// scene.h for the data structs). The EventSystem evaluates them every
// simulation step and emits a Diff for the renderer plus side effects (flags,
// dialogue). It never calls GL.
//
// Determinism: triggers are pure functions of sim state (entity positions and
// flags). Same state in → same events fire. No wall-clock, no rand().
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "world_state.h"
#include "input.h"
#include "scene.h"

namespace cv {

class EventSystem {
public:
    // Snapshot the scene's entity instances and attributes as the working logical
    // state and clear all flags. Call once after the scene is loaded.
    void init(const Scene& scene);

    // Evaluate all events against the given entity positions (keyed by id) at
    // sim time `now`, with the abstract input actions resolved this frame.
    // Appends upserts/removals to `out` and applies flag/dialogue side effects.
    // `now` lets set_motion rebase from current position. The `input` trigger
    // reads `actions`; `set_controlled`/`toggle_controlled` mutate the attrs.
    void update(Scene& scene,
                float now,
                const std::unordered_map<EntityId, Vec3>& positions,
                const ResolvedActions& actions,
                Diff& out);

    // Overload for the events demo (no keyboard) — no input actions are active.
    void update(Scene& scene,
                float now,
                const std::unordered_map<EntityId, Vec3>& positions,
                Diff& out) {
        update(scene, now, positions, ResolvedActions{}, out);
    }

    bool flag(const std::string& name) const {
        auto it = flags_.find(name);
        return it != flags_.end() && it->second;
    }

    // The working per-entity attribute store — movement reads it each frame and
    // the *_controlled actions mutate it. Initialised from the scene in init().
    std::unordered_map<EntityId, EntityAttrs>&       attrs()       { return attrs_; }
    const std::unordered_map<EntityId, EntityAttrs>& attrs() const { return attrs_; }

    // The working instance copy — movement rebases velocity on it each frame.
    std::unordered_map<EntityId, Instance>&       entities()       { return entities_; }
    const std::unordered_map<EntityId, Instance>& entities() const { return entities_; }

private:
    bool conditionPasses(const Condition& c) const;
    void runActions(const Scene& scene, float now,
                    const std::unordered_map<EntityId, Vec3>& positions,
                    const std::vector<Action>& actions, Diff& out);

    bool firstStep_ = true;   // Start triggers fire only on the first update
    std::unordered_map<std::string, bool> flags_;
    std::unordered_map<EntityId, Instance> entities_;  // working copy for set_anim
    std::unordered_map<EntityId, EntityAttrs> attrs_;  // controlled/speed per entity
};

} // namespace cv
