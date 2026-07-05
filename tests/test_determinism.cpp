// test_determinism.cpp — replay determinism of the fixed-tick simulation.
//
// Runs the pure CPU sim loop (ticks, no renderer) twice from identical initial
// SimState with an identical scripted tick-stamped input sequence and asserts
// the final states are BIT-identical. Then runs once more with the same inputs
// delivered across a different frame batching (1 tick per "frame" vs 4) —
// catching any hidden frame-rate dependence.
#include "test_harness.h"

#include "scene.h"
#include "sim.h"

#include <cstring>

using namespace cv;

namespace {

// A scene with a controlled player, a drifting walker, a proximity event that
// deflects the walker, and an input-toggled buddy.
Scene buildScene() {
    Scene s;

    Instance player = makeBillboard({0, 0.45f, 5}, {0.9f, 0.9f});
    s.initialState.instances[1] = player;
    s.nameToId["player"] = 1;
    s.attrs[1] = EntityAttrs{true, 3.0f};

    Instance walker = makeBillboard({6, 0.45f, 5}, {0.9f, 0.9f});
    walker.vel = {-0.8f, 0, 0};              // drifts toward the player
    s.initialState.instances[2] = walker;
    s.nameToId["walker"] = 2;
    s.attrs[2] = EntityAttrs{};

    Instance buddy = makeBillboard({2, 0.45f, 7}, {0.9f, 0.9f});
    s.initialState.instances[3] = buddy;
    s.nameToId["buddy"] = 3;
    s.attrs[3] = EntityAttrs{false, 2.0f};

    // start: raise a flag
    Event start;
    start.trigger.type = Trigger::Type::Start;
    Action f1; f1.type = Action::Type::SetFlag; f1.flag = "begun"; f1.value = true;
    start.actions.push_back(f1);
    s.events.push_back(start);

    // proximity player↔walker: walker leaps away, flag set
    Event prox;
    prox.trigger.type   = Trigger::Type::Proximity;
    prox.trigger.entity = "player";
    prox.trigger.target = "walker";
    prox.trigger.radius = 1.5f;
    Action leap; leap.type = Action::Type::SetMotion; leap.entity = "walker";
    leap.vel = {2.0f, 3.0f, 0}; leap.accel = {0, -5.0f, 0};
    Action f2; f2.type = Action::Type::SetFlag; f2.flag = "met_walker";
    prox.actions.push_back(leap);
    prox.actions.push_back(f2);
    s.events.push_back(prox);

    // input toggle_extra (pressed): buddy joins/leaves the controlled set
    Event toggle;
    toggle.trigger.type   = Trigger::Type::Input;
    toggle.trigger.action = "toggle_extra";
    toggle.trigger.edge   = Trigger::Edge::Pressed;
    toggle.once = false;
    Action tg; tg.type = Action::Type::ToggleControlled; tg.entity = "buddy";
    toggle.actions.push_back(tg);
    s.events.push_back(toggle);

    return s;
}

// The scripted tick-stamped input command stream: the sim's inputs are a pure
// function of the tick number (lockstep-replay style).
ResolvedActions actionsForTick(uint64_t tick) {
    ResolvedActions a;
    // hold east from tick 10..199, then north 200..349
    if (tick >= 10 && tick < 200) a.down.insert("move_east");
    if (tick >= 200 && tick < 350) a.down.insert("move_north");
    // toggle the buddy in at tick 60 and out at tick 300 (pressed edges)
    if (tick == 60 || tick == 300) {
        a.pressed.insert("toggle_extra");
        a.down.insert("toggle_extra");
    }
    return a;
}

const uint64_t TOTAL_TICKS = 600;

// Run TOTAL_TICKS of simulation, `batch` ticks per simulated "frame". The
// batching must be invisible to the result: inputs are per-tick commands.
SimState run(const Scene& scene, uint64_t batch) {
    SimState st = makeSimState(scene);
    uint64_t done = 0;
    while (done < TOTAL_TICKS) {
        uint64_t n = batch;
        if (done + n > TOTAL_TICKS) n = TOTAL_TICKS - done;
        for (uint64_t i = 0; i < n; ++i) {   // the "frame" runs n ticks
            Diff diff;
            stepSim(scene, st, actionsForTick(st.clock.tick), diff);
            ++done;
        }
    }
    return st;
}

bool bitIdenticalInstances(const SimState& a, const SimState& b) {
    if (a.entities.size() != b.entities.size()) return false;
    for (const auto& kv : a.entities) {
        auto it = b.entities.find(kv.first);
        if (it == b.entities.end()) return false;
        if (std::memcmp(&kv.second, &it->second, sizeof(Instance)) != 0)
            return false;
    }
    return true;
}

bool sameFlags(const SimState& a, const SimState& b) {
    if (a.flags.size() != b.flags.size()) return false;
    for (const auto& kv : a.flags) {
        auto it = b.flags.find(kv.first);
        if (it == b.flags.end() || it->second != kv.second) return false;
    }
    return true;
}

} // namespace

void test_determinism() {
    Scene scene = buildScene();

    SimState a = run(scene, 1);
    SimState b = run(scene, 1);

    // Identical runs → bit-identical results.
    CHECK(a.clock.tick == TOTAL_TICKS && b.clock.tick == TOTAL_TICKS);
    CHECK(bitIdenticalInstances(a, b));
    CHECK(sameFlags(a, b));
    CHECK(a.fired == b.fired);

    // Sanity: the scripted run actually exercised the systems.
    CHECK(a.flags.count("begun") == 1);
    CHECK(a.flags.count("met_walker") == 1);      // proximity fired
    CHECK(a.fired.size() == 3 && a.fired[0] && a.fired[1]);
    CHECK(a.entities.at(1).pos.x > 0.0f);         // player moved east

    // Different frame batching (4 ticks per "frame") → identical results.
    // This is the frame-rate-independence guarantee the shim system needs.
    SimState c = run(scene, 4);
    CHECK(c.clock.tick == TOTAL_TICKS);
    CHECK(bitIdenticalInstances(a, c));
    CHECK(sameFlags(a, c));
    CHECK(a.fired == c.fired);

    // And an uneven batch size that doesn't divide TOTAL_TICKS.
    SimState d = run(scene, 7);
    CHECK(bitIdenticalInstances(a, d));
    CHECK(sameFlags(a, d));
    CHECK(a.fired == d.fired);

    // Forkability: copying a mid-run SimState and stepping both copies with the
    // same commands stays bit-identical (the shim lookahead's core operation).
    {
        SimState live = makeSimState(scene);
        for (int i = 0; i < 100; ++i) {
            Diff diff;
            stepSim(scene, live, actionsForTick(live.clock.tick), diff);
        }
        SimState fork = live;                     // cheap copy, no scene/renderer refs
        for (int i = 0; i < 100; ++i) {
            Diff d1, d2;
            stepSim(scene, live, actionsForTick(live.clock.tick), d1);
            stepSim(scene, fork, actionsForTick(fork.clock.tick), d2);
        }
        CHECK(bitIdenticalInstances(live, fork));
        CHECK(sameFlags(live, fork));
        CHECK(live.fired == fork.fired);
    }
}
