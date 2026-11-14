// world_state.h — the data the engine renders, plus the diff that mutates it.
//
// This is the injection seam. The demo builds a WorldState directly; a future
// game (or a JSON-from-webpage adapter) builds Diffs. The renderer consumes both
// and knows nothing about where they came from.
#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "camera.h"
#include "instance.h"

namespace cv {

using EntityId = uint32_t;

// Optional sky backdrop: a vertical gradient evaluated per pixel from the view
// ray, so the horizon stays put as the camera pitches and turns. When disabled
// the renderer clears to its flat background colour instead.
struct Sky {
    bool enabled = false;
    Vec3 zenith  = {0.20f, 0.42f, 0.80f};  // straight up
    Vec3 horizon = {0.74f, 0.86f, 0.96f};  // at eye level
    Vec3 ground  = {0.16f, 0.20f, 0.18f};  // below the horizon (beyond the map)
};

// A complete snapshot of the renderable world.
struct WorldState {
    std::unordered_map<EntityId, Instance> instances;
    std::vector<Camera> cameras;
    int activeCamera = 0;
    Sky sky;
};

// An incremental change. Apply on top of current renderer state.
//   upserts:    add new instances or replace existing ones (by id)
//   removals:   ids to delete
//   camera/active: optional camera edits
struct Diff {
    std::vector<std::pair<EntityId, Instance>> upserts;
    std::vector<EntityId> removals;

    bool setActiveCamera = false;
    int activeCamera = 0;

    // Optional whole-camera replacement (e.g. moving the orbiting camera).
    bool replaceCameras = false;
    std::vector<Camera> cameras;
};

} // namespace cv
