// sdl_input.cpp — SDL keyboard events → device-agnostic InputFrame.
#include "sdl_input.h"

#include <cctype>
#include <string>

namespace cv {

// Map an SDL key event to the device-agnostic lowercase key name the input
// layer uses for bindings. Letters/digits pass through lowercased; arrows and
// space get stable names. Returns "" for keys we don't expose.
static std::string keyName(const SDL_Keysym& k) {
    switch (k.sym) {
        case SDLK_UP:    return "up";
        case SDLK_DOWN:  return "down";
        case SDLK_LEFT:  return "left";
        case SDLK_RIGHT: return "right";
        case SDLK_SPACE: return "space";
        default: break;
    }
    std::string name = SDL_GetKeyName(k.sym);  // e.g. "W", "Escape"
    if (name.size() != 1) return "";           // only single-character keys here
    return std::string(1, static_cast<char>(std::tolower(name[0])));
}

InputFrame buildInputFrame(const std::vector<SDL_Event>& events,
                           const InputFrame& prev) {
    InputFrame frame;
    frame.down = prev.down;  // carry over held keys

    for (const SDL_Event& e : events) {
        if (e.type == SDL_KEYDOWN && e.key.repeat == 0) {
            std::string name = keyName(e.key.keysym);
            if (!name.empty()) {
                frame.pressed.insert(name);
                frame.down.insert(name);
            }
        } else if (e.type == SDL_KEYUP) {
            std::string name = keyName(e.key.keysym);
            if (!name.empty()) {
                frame.released.insert(name);
                frame.down.erase(name);
            }
        }
    }
    return frame;
}

} // namespace cv
