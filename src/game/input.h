// input.h — keyboard input layer for the data-driven scene.
//
// Three concerns live here, all device-agnostic and deterministic:
//
//   InputFrame  — raw per-frame keyboard state, expressed as lowercase key
//                 names ("w", "left", "space"). The engine fills this from SDL
//                 each frame; nothing here calls SDL so the layer ports cleanly.
//   Bindings    — key name → abstract action ("w" → "move_north"), parsed from a
//                 scene "controls" block. Resolving the InputFrame through the
//                 bindings yields the set of actions down/pressed/released.
//   movement    — given the resolved actions and the per-entity attribute store,
//                 apply a velocity to every controlled entity (free movement,
//                 not grid-locked) and emit upserts into a Diff.
//
// Movement is FREE: the 90° grid lock applies to the camera, not characters.
#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>

#include "world_state.h"

namespace cv {

// Per-entity, game-side attributes that never touch the GPU Instance (which must
// stay a standard-layout block of floats). Lives in the EventSystem alongside the
// working instances, initialised from the scene.
struct EntityAttrs {
    bool  controlled = false;   // movement input applies to every controlled entity
    float speed      = 3.0f;    // world units / second when moving
};

// Raw keyboard state for one frame. Key names are lowercase and device-agnostic
// (letters as-is; arrows as "up"/"down"/"left"/"right"; space as "space").
struct InputFrame {
    std::unordered_set<std::string> down;       // keys currently held
    std::unordered_set<std::string> pressed;    // keys that went down this frame
    std::unordered_set<std::string> released;   // keys that went up this frame
};

// key name → abstract action, parsed from the scene "controls.bindings" block.
using Bindings = std::unordered_map<std::string, std::string>;

// Abstract actions resolved from an InputFrame through the Bindings. The event
// system's `input` trigger reads these edge sets; movement reads `down`.
struct ResolvedActions {
    std::unordered_set<std::string> down;
    std::unordered_set<std::string> pressed;
    std::unordered_set<std::string> released;

    bool isDown(const std::string& a) const { return down.count(a) != 0; }
};

// Map each key in `in` through `bindings` to its abstract action. A key with no
// binding contributes nothing.
ResolvedActions resolveActions(const InputFrame& in, const Bindings& bindings);

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
