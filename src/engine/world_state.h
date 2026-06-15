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

// A complete snapshot of the renderable world.
struct WorldState {
    std::unordered_map<EntityId, Instance> instances;
    std::vector<Camera> cameras;
    int activeCamera = 0;
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
