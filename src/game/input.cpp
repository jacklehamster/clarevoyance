// input.cpp — InputFrame → abstract actions.
//
// SDL-free: the engine translates SDL events into an InputFrame
// (src/engine/sdl_input.cpp) before anything here runs.
#include "input.h"

namespace cv {

ResolvedActions resolveActions(const InputFrame& in, const Bindings& bindings) {
    ResolvedActions out;
    auto map = [&](const std::unordered_set<std::string>& keys,
                   std::unordered_set<std::string>& dst) {
        for (const std::string& key : keys) {
            auto it = bindings.find(key);
            if (it != bindings.end()) dst.insert(it->second);
        }
    };
    map(in.down,     out.down);
    map(in.pressed,  out.pressed);
    map(in.released, out.released);
    return out;
}

} // namespace cv
