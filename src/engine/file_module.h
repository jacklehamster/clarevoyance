#pragma once
#include <ctime>
#include <string>
#include "module.h"

namespace cv {

// Loads a JSON scene file and injects scripts into the runner.
// Hot-reloads automatically when the file changes on disk — edit the JSON
// and the scene updates on the next tick without restarting.
//
// JSON format:
// {
//   "entities": [
//     {
//       "id": 1,
//       "scale": [0.9, 0.9],        // optional, default [1,1]
//       "billboard": 1,             // optional, default 1
//       "anim": { "first": 0, "count": 5, "fps": 10 },  // optional
//       "phases": [
//         { "pos": [x,y,z], "vel": [x,y,z], "accel": [x,y,z], "duration": 2.0 },
//         ...
//       ],
//       "loop": true                // optional, default false
//     },
//     {
//       "id": 2,
//       "repeat": {
//         "seed": { "pos": [x,y,z], "vel": [x,y,z], "accel": [x,y,z] },
//         "damping": 0.6,
//         "minVel": 0.4
//       }
//     }
//   ]
// }
class FileModule : public Module {
public:
    explicit FileModule(const char* path);
    void tick(float simTime, ScriptRunner& runner, Diff& diff) override;

private:
    void reload(float simTime, ScriptRunner& runner, Diff& diff);

    std::string path_;
    time_t      lastMod_  = -1;
    float       loadTime_ = 0.0f;
};

} // namespace cv
