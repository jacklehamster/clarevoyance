// test_quest.cpp — end-to-end integration test for src/levels/quest.json:
// scripted movement (not hand-set flags) drives the player to each key, the
// counter-gated door, and the win zone, exercising the whole event/condition/
// action pipeline exactly as a real playthrough would.
#include "test_harness.h"

#include "scene.h"
#include "sim.h"
#include "events.h"

#include <cstdio>

using namespace cv;

namespace {

// Mirrors the private positionAt() in sim.cpp (analytic motion model) — needed
// here to track the player's real position between stepSim calls while
// scripting movement toward a target.
Vec3 positionAt(const Instance& inst, float t) {
    float dt = t - inst.motionStart;
    return inst.pos + inst.vel * dt + inst.accel * (0.5f * dt * dt);
}

// Steers the controlled `playerId` toward `target` one tick at a time (via the
// same move_north/-south/-west/-east abstract actions a real keyboard would
// produce) until within `tolerance` or `maxTicks` elapses. Returns whether it
// got there — a real scripted playthrough, not a teleport.
bool walkToward(const Scene& scene, SimState& st, EntityId playerId,
                Vec3 target, float tolerance, int maxTicks) {
    for (int i = 0; i < maxTicks; ++i) {
        Vec3 pos = positionAt(st.entities.at(playerId),
                              static_cast<float>(st.clock.time()));
        Vec3 d = target - pos;
        if (length(d) <= tolerance) return true;

        ResolvedActions actions;
        if (d.x >  0.05f) actions.down.insert("move_east");
        if (d.x < -0.05f) actions.down.insert("move_west");
        if (d.z >  0.05f) actions.down.insert("move_south");
        if (d.z < -0.05f) actions.down.insert("move_north");

        Diff diff;
        stepSim(scene, st, actions, diff);
    }
    Vec3 pos = positionAt(st.entities.at(playerId),
                          static_cast<float>(st.clock.time()));
    return length(target - pos) <= tolerance;
}

} // namespace

void test_quest() {
    Scene scene;
    std::string err;
    bool ok = loadScene("src/levels/quest.json", scene, err);
    CHECK_MSG(ok, err.c_str());
    if (!ok) return;

    EntityId player = scene.idOf("player");
    CHECK(player != 0);

    SimState st = makeSimState(scene);
    // Start trigger tick (none defined in quest.json, but this establishes
    // state.started the same way the real game loop's first frame would).
    { Diff d0; stepSim(scene, st, ResolvedActions{}, d0); }

    // --- Key 1 ----------------------------------------------------------
    CHECK(walkToward(scene, st, player, {1.0f, 0.9f, 2.0f}, 0.4f, 600));
    CHECK(st.flags["keys"] == 1.0);
    CHECK(st.entities.count(scene.idOf("key1")) == 0);   // despawned

    // --- Key 2 ----------------------------------------------------------
    CHECK(walkToward(scene, st, player, {5.0f, 0.9f, 2.0f}, 0.4f, 900));
    CHECK(st.flags["keys"] == 2.0);
    CHECK(st.entities.count(scene.idOf("key2")) == 0);

    // --- Door does NOT open with only 2 keys -----------------------------
    // Probe on a throwaway COPY of the sim state (SimState is cheaply
    // copyable by design — the same property the shim lookahead relies on):
    // teleport the copy's player right up against the door and step once.
    // The condition (keys >= 3) must block the removal even though the
    // proximity trigger itself is satisfied.
    EntityId doorId = scene.idOf("door");
    {
        SimState probe = st;
        probe.entities[player].pos         = {3.0f, 0.9f, 6.0f};
        probe.entities[player].vel         = {0, 0, 0};
        probe.entities[player].motionStart = static_cast<float>(probe.clock.time());
        Diff dp;
        stepSim(scene, probe, ResolvedActions{}, dp);
        CHECK(probe.entities.count(doorId) == 1);    // still there — condition gates it
        CHECK(probe.flags.count("door_open") == 0);
    }

    // --- Key 3 ------------------------------------------------------------
    CHECK(walkToward(scene, st, player, {3.0f, 0.9f, 4.5f}, 0.4f, 900));
    CHECK(st.flags["keys"] == 3.0);
    CHECK(st.entities.count(scene.idOf("key3")) == 0);

    // --- Door opens now that keys >= 3 ------------------------------------
    CHECK(walkToward(scene, st, player, {3.0f, 0.9f, 6.0f}, 1.0f, 900));
    CHECK(st.entities.count(doorId) == 0);      // removed
    CHECK(st.flags["door_open"] == 1.0);

    // --- Win zone: victory fires once past the open door ------------------
    CHECK(walkToward(scene, st, player, {3.0f, 0.9f, 7.3f}, 0.9f, 900));
    const Clip* chat = scene.clipOf(player, "chat");
    CHECK(chat != nullptr);
    if (chat) {
        const Instance& p = st.entities.at(player);
        CHECK(p.anim.x == static_cast<float>(chat->first));
        CHECK(p.anim.y == static_cast<float>(chat->count));
    }
}
