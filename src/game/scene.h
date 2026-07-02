// scene.h — the data-driven scene: entities, cameras, and events loaded from JSON.
//
// A Scene is the parsed, validated form of a level data file. It owns the
// string-name → numeric EntityId mapping (data files reference entities by name;
// the renderer wants integer ids) and produces the initial WorldState the
// renderer uploads. Events are held here and run by the EventSystem each step.
//
// This is the "script layer" seam: nothing here calls GL. It builds engine
// data structures (WorldState / Instance / Camera) from declarative data.
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "world_state.h"
#include "events.h"
#include "input.h"

namespace cv {

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

} // namespace cv
