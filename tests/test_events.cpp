// test_events.cpp — event system semantics over an in-code Scene:
// start/proximity/input triggers, condition gating, once semantics, and the
// set_flag / toggle_controlled / set_controlled / set_anim / remove actions.
#include "test_harness.h"

#include "scene.h"
#include "sim.h"

using namespace cv;

namespace {

// player (id 1, controlled, speed 2) at origin; mochi (id 2) at (10,0,0).
Scene buildScene(Vec3 playerPos = {0, 0, 0}) {
    Scene s;
    s.initialState.instances[1] = makeBillboard(playerPos, {1, 1});
    s.nameToId["player"] = 1;
    s.attrs[1] = EntityAttrs{true, 2.0f};

    s.initialState.instances[2] = makeBillboard({10, 0, 0}, {1, 1});
    s.nameToId["mochi"] = 2;
    s.attrs[2] = EntityAttrs{};
    return s;
}

Action setFlag(const std::string& flag, bool value = true) {
    Action a;
    a.type  = Action::Type::SetFlag;
    a.flag  = flag;
    a.value = value;
    return a;
}

Action toggleControlled(const std::string& entity) {
    Action a;
    a.type   = Action::Type::ToggleControlled;
    a.entity = entity;
    return a;
}

Event startEvent(Action a, bool once = true) {
    Event e;
    e.trigger.type = Trigger::Type::Start;
    e.once = once;
    e.actions.push_back(std::move(a));
    return e;
}

Event proximityEvent(float radius, Action a, bool once = true) {
    Event e;
    e.trigger.type   = Trigger::Type::Proximity;
    e.trigger.entity = "player";
    e.trigger.target = "mochi";
    e.trigger.radius = radius;
    e.once = once;
    e.actions.push_back(std::move(a));
    return e;
}

Event inputEvent(const std::string& action, Trigger::Edge edge,
                 Action a, bool once = true) {
    Event e;
    e.trigger.type   = Trigger::Type::Input;
    e.trigger.action = action;
    e.trigger.edge   = edge;
    e.once = once;
    e.actions.push_back(std::move(a));
    return e;
}

// Step one tick with no input.
void stepIdle(const Scene& s, SimState& st) {
    Diff diff;
    stepSim(s, st, ResolvedActions{}, diff);
}

} // namespace

void test_events() {
    // --- Start trigger fires on the FIRST step only --------------------------
    {
        Scene s = buildScene();
        // once=false so a refire would be observable: each fire toggles mochi.
        s.events.push_back(startEvent(toggleControlled("mochi"), /*once=*/false));
        SimState st = makeSimState(s);
        CHECK(!st.attrs[2].controlled);
        stepIdle(s, st);
        CHECK(st.attrs[2].controlled);        // fired on first step
        stepIdle(s, st);
        stepIdle(s, st);
        CHECK(st.attrs[2].controlled);        // never fired again (still toggled once)
        CHECK(st.fired.size() == 1 && st.fired[0] == 1);
    }

    // --- Proximity fires when within radius, not when outside ----------------
    {
        Scene sFar = buildScene({0, 0, 0});   // distance 10 > radius 1.5
        sFar.events.push_back(proximityEvent(1.5f, setFlag("met")));
        SimState st = makeSimState(sFar);
        stepIdle(sFar, st);
        CHECK(st.flags.count("met") == 0);

        Scene sNear = buildScene({9, 0, 0});  // distance 1 <= radius 1.5
        sNear.events.push_back(proximityEvent(1.5f, setFlag("met")));
        SimState st2 = makeSimState(sNear);
        stepIdle(sNear, st2);
        CHECK(st2.flags.count("met") == 1 && st2.flags["met"]);
    }

    // --- once=true: trigger keeps being true but fires only once -------------
    {
        Scene s = buildScene({9, 0, 0});      // permanently in range
        s.events.push_back(proximityEvent(1.5f, toggleControlled("mochi"),
                                          /*once=*/true));
        SimState st = makeSimState(s);
        stepIdle(s, st);
        stepIdle(s, st);
        stepIdle(s, st);
        CHECK(st.attrs[2].controlled);        // toggled exactly once
    }

    // --- once=false: refires every tick the trigger holds --------------------
    {
        Scene s = buildScene({9, 0, 0});
        s.events.push_back(proximityEvent(1.5f, toggleControlled("mochi"),
                                          /*once=*/false));
        SimState st = makeSimState(s);
        stepIdle(s, st);
        CHECK(st.attrs[2].controlled);        // tick 1: toggled on
        stepIdle(s, st);
        CHECK(!st.attrs[2].controlled);       // tick 2: toggled off again
    }

    // --- Condition gating -----------------------------------------------------
    {
        Scene s = buildScene({9, 0, 0});
        Event e = proximityEvent(1.5f, setFlag("done"));
        e.condition.present = true;
        e.condition.flag    = "gate";
        e.condition.value   = true;
        s.events.push_back(e);
        SimState st = makeSimState(s);
        stepIdle(s, st);
        CHECK(st.flags.count("done") == 0);   // gate is false → no fire
        st.flags["gate"] = true;
        stepIdle(s, st);
        CHECK(st.flags.count("done") == 1);   // gate open → fires
    }

    // --- Input trigger: pressed fires once, held-only ticks do not refire ----
    {
        Scene s = buildScene();
        s.events.push_back(inputEvent("jump", Trigger::Edge::Pressed,
                                      toggleControlled("mochi"), /*once=*/false));
        SimState st = makeSimState(s);

        ResolvedActions press;                // the edge tick
        press.pressed.insert("jump");
        press.down.insert("jump");
        Diff d1;
        stepSim(s, st, press, d1);
        CHECK(st.attrs[2].controlled);        // fired on the pressed edge

        ResolvedActions held;                 // subsequent tick: held, no edge
        held.down.insert("jump");
        Diff d2;
        stepSim(s, st, held, d2);
        CHECK(st.attrs[2].controlled);        // did NOT refire (still toggled once)

        Diff d3;                              // release then press again → refires
        ResolvedActions press2 = press;
        stepSim(s, st, press2, d3);
        CHECK(!st.attrs[2].controlled);
    }

    // --- Input trigger: released and held edges -------------------------------
    {
        Scene s = buildScene();
        s.events.push_back(inputEvent("jump", Trigger::Edge::Released,
                                      setFlag("let_go")));
        s.events.push_back(inputEvent("jump", Trigger::Edge::Held,
                                      setFlag("holding")));
        SimState st = makeSimState(s);

        ResolvedActions held;
        held.down.insert("jump");
        Diff d1;
        stepSim(s, st, held, d1);
        CHECK(st.flags.count("holding") == 1);
        CHECK(st.flags.count("let_go") == 0);

        ResolvedActions release;
        release.released.insert("jump");
        Diff d2;
        stepSim(s, st, release, d2);
        CHECK(st.flags.count("let_go") == 1);
    }

    // --- set_controlled / set_anim / remove actions ---------------------------
    {
        Scene s = buildScene();
        Action setCtl;
        setCtl.type   = Action::Type::SetControlled;
        setCtl.entity = "mochi";
        setCtl.value  = true;
        Action setAnim;
        setAnim.type   = Action::Type::SetAnim;
        setAnim.entity = "mochi";
        setAnim.first  = 6;
        setAnim.count  = 2;
        setAnim.fps    = 8.0f;
        Action remove;
        remove.type   = Action::Type::Remove;
        remove.entity = "player";
        Event e = startEvent(setCtl);
        e.actions.push_back(setAnim);
        e.actions.push_back(remove);
        s.events.push_back(e);

        SimState st = makeSimState(s);
        Diff diff;
        stepSim(s, st, ResolvedActions{}, diff);
        CHECK(st.attrs[2].controlled);
        CHECK(st.entities[2].anim.x == 6.0f && st.entities[2].anim.y == 2.0f
              && st.entities[2].anim.z == 8.0f);
        CHECK(st.entities.count(1) == 0);     // player removed from sim state
        CHECK(diff.removals.size() == 1 && diff.removals[0] == 1);
        bool sawAnimUpsert = false;
        for (const auto& up : diff.upserts)
            if (up.first == 2 && up.second.anim.x == 6.0f) sawAnimUpsert = true;
        CHECK(sawAnimUpsert);
    }

    // --- Movement: held directional action applies velocity to controlled ----
    {
        Scene s = buildScene();
        SimState st = makeSimState(s);
        ResolvedActions east;
        east.down.insert("move_east");
        Diff diff;
        stepSim(s, st, east, diff);
        CHECK(st.entities[1].vel.x == 2.0f);  // player speed
        CHECK(st.entities[2].vel.x == 0.0f);  // mochi not controlled
        Diff diff2;                            // steady motion → no upsert churn
        stepSim(s, st, east, diff2);
        CHECK(diff2.upserts.empty());
    }
}
