// sdl_input.h — the engine-side SDL → InputFrame translation.
//
// This is the ONLY place SDL keyboard events meet the game input layer. The
// engine collects SDL events each rendered frame and calls buildInputFrame to
// produce the device-agnostic InputFrame (lowercase key names, down/pressed/
// released sets) that the game layer consumes. Nothing under src/game/ may
// include SDL headers.
#pragma once

#include <vector>

#include <SDL2/SDL.h>

#include "input.h"

namespace cv {

// Build this frame's InputFrame from the SDL events collected since the last
// frame. `prev` is the previous frame's InputFrame: its `down` set carries held
// keys across frames; pressed/released edges are derived from the new events
// only. Key repeats are ignored (a held key stays in `down`, no new edge).
InputFrame buildInputFrame(const std::vector<SDL_Event>& events,
                           const InputFrame& prev);

} // namespace cv
