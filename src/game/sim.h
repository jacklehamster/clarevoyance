// sim.h — the fixed-timestep simulation clock.
//
// The game simulation advances in fixed ticks of SIM_DT seconds (60 Hz — owner
// decision, see docs/specs/SHIM_SYSTEM.md). Sim time is DERIVED from the integer
// tick counter, never from the wall clock, so the same input sequence produces
// the same trigger firings at any frame rate on desktop, WASM, and Switch.
//
// The engine's render loop runs an accumulator over wall time and steps the sim
// zero or more whole ticks per rendered frame. Rendering stays smooth because
// the renderer receives the CONTINUOUS time `clock.time() + accumulator
// remainder` — the analytic motion model (pos + vel*t + ½·accel·t²) makes that
// exact, not an approximation.
//
// Networking note: the per-tick input is conceptually a tick-stamped command —
// "resolve actions for tick N" stays a pure function of (command, bindings), so
// a lockstep peer can replay the identical command stream deterministically.
#pragma once

#include <cstdint>

namespace cv {

// Fixed simulation timestep: 60 Hz.
constexpr double SIM_DT = 1.0 / 60.0;

// Integer tick counter; sim time is derived, never stored, so it cannot drift.
struct SimClock {
    uint64_t tick = 0;
    double time() const { return static_cast<double>(tick) * SIM_DT; }
};

} // namespace cv
