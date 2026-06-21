// events.h — the data-driven event/condition/action system.
//
// Game logic (dialogue, flag changes, animation swaps) is expressed as data:
// each Event is a Trigger + an optional Condition + a list of Actions. The
// EventSystem evaluates them every simulation step and emits a Diff for the
// renderer plus side effects (flags, dialogue). It never calls GL.
//
// Determinism: triggers are pure functions of sim state (entity positions and
// flags). Same state in → same events fire. No wall-clock, no rand().
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "world_state.h"

namespace cv {

struct Trigger {
    enum class Type { Start, Proximity };
    Type type = Type::Start;
    std::string entity;     // proximity: the moving entity (by data-file name)
    std::string target;     // proximity: the entity it approaches
    float radius = 1.0f;    // proximity: fire when distance <= radius
};

struct Condition {
    bool present = false;   // no condition → always passes
    std::string flag;
    bool value = true;      // condition passes when flags[flag] == value
};

struct Action {
    enum class Type { Dialogue, SetFlag, SetAnim, SetMotion, Remove };
    Type type = Type::Dialogue;

    std::string id;         // dialogue: line id
    std::string flag;       // set_flag: flag name
    bool value = true;      // set_flag: value

    std::string entity;     // set_anim / set_motion / remove: target entity name
    int   first = 0;        // set_anim: first frame
    int   count = 1;        // set_anim: frame count
    float fps   = 0.0f;     // set_anim: frames per second

    Vec3  vel   = {0, 0, 0}; // set_motion: new velocity
    Vec3  accel = {0, 0, 0}; // set_motion: new acceleration
};

struct Event {
    Trigger trigger;
    Condition condition;
    std::vector<Action> actions;
    bool once = true;       // fire at most once
    bool fired = false;     // runtime guard
};

struct Scene;  // forward decl (scene.h includes this header)

class EventSystem {
public:
    // Snapshot the scene's entity instances as the working logical state and
    // clear all flags. Call once after the scene is loaded.
    void init(const Scene& scene);

    // Evaluate all events against the given entity positions (keyed by id) at
    // sim time `now`. Appends upserts/removals to `out` and applies flag/
    // dialogue side effects. `now` lets set_motion rebase from current position.
    void update(Scene& scene,
                float now,
                const std::unordered_map<EntityId, Vec3>& positions,
                Diff& out);

    bool flag(const std::string& name) const {
        auto it = flags_.find(name);
        return it != flags_.end() && it->second;
    }

private:
    bool conditionPasses(const Condition& c) const;
    void runActions(Scene& scene, float now,
                    const std::unordered_map<EntityId, Vec3>& positions,
                    const std::vector<Action>& actions, Diff& out);

    std::unordered_map<std::string, bool> flags_;
    std::unordered_map<EntityId, Instance> entities_;  // working copy for set_anim
};

} // namespace cv
