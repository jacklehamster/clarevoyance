// scene.h — the data-driven scene: entities, cameras, and events loaded from JSON.
//
// A Scene is the parsed, validated form of a level data file. It owns the
// string-name → numeric EntityId mapping (data files reference entities by name;
// the renderer wants integer ids) and produces the initial WorldState the
// renderer uploads. Events are held here and run by the EventSystem each step.
//
// This is the "script layer" seam: nothing here calls GL. It builds engine
// data structures (WorldState / Instance / Camera) from declarative data.
//
// A Scene is DATA: after loadScene() it is immutable. All runtime mutation
// (entity motion, flags, event fired markers, the sim clock) lives in the
// copyable SimState (sim.h), never in the Scene itself — required so the
// shim lookahead can fork the sim without touching the scene.
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "world_state.h"
#include "input.h"

namespace cv {

// Per-entity, game-side attributes that never touch the GPU Instance (which must
// stay a standard-layout block of floats). Parsed from the entity JSON; the
// EventSystem keeps a mutable working copy alongside the working instances.
struct EntityAttrs {
    bool  controlled = false;   // movement input applies to every controlled entity
    float speed      = 3.0f;    // world units / second when moving
};

// --- Event data (trigger / condition / action rules) -------------------------
// Pure data parsed from the scene file; evaluated by the EventSystem.

struct Trigger {
    enum class Type { Start, Proximity, Input };
    enum class Edge { Pressed, Released, Held };   // input: which edge fires
    Type type = Type::Start;
    std::string entity;     // proximity: the moving entity (by data-file name)
    std::string target;     // proximity: the entity it approaches
    float radius = 1.0f;    // proximity: fire when distance <= radius

    std::string action;     // input: the abstract action name to watch
    Edge edge = Edge::Pressed;  // input: fire on pressed / released / held
};

struct Condition {
    bool present = false;   // no condition → always passes
    std::string flag;
    bool value = true;      // condition passes when flags[flag] == value
};

struct Action {
    enum class Type { Dialogue, SetFlag, SetAnim, SetMotion, Remove,
                      ToggleControlled, SetControlled };
    Type type = Type::Dialogue;

    std::string id;         // dialogue: line id
    std::string flag;       // set_flag: flag name
    bool value = true;      // set_flag / set_controlled: value

    std::string entity;     // set_anim / set_motion / remove / *_controlled: target
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
    bool once = true;       // fire at most once (fired markers live in SimState)
};

// Sprite-sheet description pulled from the scene file (the renderer needs it at init).
struct SheetInfo {
    std::string path = "art/penguin.png";
    int cols = 16;
    int rows = 1;
};

struct Scene {
    SheetInfo sheet;
    WorldState initialState;                 // entities + cameras, ready to upload
    std::vector<Event> events;               // trigger/condition/action rules
    std::unordered_map<std::string, EntityId> nameToId;  // data-file name → id
    Bindings bindings;                       // key name → abstract action ("controls")
    std::unordered_map<EntityId, EntityAttrs> attrs;  // per-entity game-side attributes

    // Resolve a data-file entity name to its numeric id. Returns 0 if unknown
    // (0 is never assigned to a real entity — ids start at 1).
    EntityId idOf(const std::string& name) const {
        auto it = nameToId.find(name);
        return it == nameToId.end() ? 0 : it->second;
    }
};

// Parse a scene JSON file. On failure returns false and fills `error`.
bool loadScene(const char* path, Scene& out, std::string& error);

// Apply directional movement to every controlled entity.
//
// Held directional actions (move_north/-south/-west/-east) sum into a world-space
// direction (+X east, +Z south, +Y up): north = -Z, south = +Z, west = -X,
// east = +X. The direction is normalised, scaled by each entity's speed, and
// applied as velocity using the same rebase-to-current-position + motionStart=now
// approach as the set_motion action. An upsert is emitted only when an entity's
// desired velocity actually differs from its current velocity (no per-frame churn).
void applyMovement(const ResolvedActions& actions,
                   const std::unordered_map<EntityId, EntityAttrs>& attrs,
                   std::unordered_map<EntityId, Instance>& entities,
                   float now,
                   const std::unordered_map<EntityId, Vec3>& positions,
                   Diff& out);

} // namespace cv
