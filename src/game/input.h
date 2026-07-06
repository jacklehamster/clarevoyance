// input.h — device-agnostic input layer for the data-driven scene.
//
// Two concerns live here, both deterministic and free of any SDL dependency:
//
//   InputFrame  — raw per-frame keyboard state, expressed as lowercase key
//                 names ("w", "left", "space"). The ENGINE builds this from SDL
//                 events (src/engine/sdl_input.h) and hands it to the game
//                 layer; nothing here calls SDL so the layer ports cleanly.
//   Bindings    — key name → abstract action ("w" → "move_north"), parsed from a
//                 scene "controls" block. Resolving the InputFrame through the
//                 bindings yields the set of actions down/pressed/released.
//
// The movement pass (applying velocity to controlled entities) lives with the
// simulation, not here — see scene.h / applyMovement.
#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>

namespace cv {

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
// binding contributes nothing. Pure function — same frame + same bindings in,
// same actions out.
ResolvedActions resolveActions(const InputFrame& in, const Bindings& bindings);

// Abstract input source. Implement to feed ResolvedActions from any origin:
// keyboard bindings, replay file, network, or nothing (cutscenes). The engine
// builds the InputFrame (from SDL, a replay, …) and passes it in; poll never
// touches a device directly.
struct InputSource {
    virtual ~InputSource() = default;
    virtual ResolvedActions poll(const InputFrame& frame) = 0;
};

// Resolves the passed frame through a bindings table. Holds a COPY of the
// bindings so the source stays valid if the owning Scene is unloaded or
// hot-reloaded out from under it.
struct BindingsInputSource : InputSource {
    Bindings bindings;
    explicit BindingsInputSource(Bindings b) : bindings(std::move(b)) {}
    ResolvedActions poll(const InputFrame& frame) override {
        return resolveActions(frame, bindings);
    }
};

// No-op: always returns empty action sets. Use for cutscenes and replays.
struct NullInputSource : InputSource {
    ResolvedActions poll(const InputFrame&) override { return {}; }
};

} // namespace cv
