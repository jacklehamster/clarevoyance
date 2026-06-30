#pragma once
#include "script.h"
#include "world_state.h"

namespace cv {

// Abstract driver that injects scripts and entity changes each tick.
// Swap implementations to change what drives the scene:
//   FileModule     — loads a JSON scene file, hot-reloads on change (testing)
//   GameModule     — actual game logic (combat, AI, events)
//   WebStreamModule — streams scene data from a remote source
class Module {
public:
    virtual ~Module() = default;
    virtual void tick(float simTime, ScriptRunner& runner, Diff& diff) = 0;
};

} // namespace cv
