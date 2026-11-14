// test_world.cpp — the explorable world: terrain (authored map + procedural
// wilds), collision, the follow-camera rig, labels and patrols.
#include "test_harness.h"

#include "camera_rig.h"
#include "scene.h"
#include "sim.h"
#include "terrain.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>

using namespace cv;
namespace fs = std::filesystem;

namespace {

bool loadFromString(const std::string& json, Scene& out, std::string& err) {
    fs::create_directories("build");
    const char* path = "build/test_world_tmp.json";
    std::FILE* f = std::fopen(path, "wb");
    if (!f) { err = "cannot open temp file"; return false; }
    std::fwrite(json.data(), 1, json.size(), f);
    std::fclose(f);
    bool ok = loadScene(path, out, err);
    fs::remove(path);
    return ok;
}

// A small walled yard in a procedural wilderness, with a hero and a patrol.
//
//      x: 0123456        '#' wall (2 high)   '.' floor   '~' water
//   z=0   #######        'H' hero spawn      'E' patrolling enemy (+2 east)
//   z=1   #H....#
//   z=2   #..~..#
//   z=3   #E....#
//   z=4   ###.###        (a gap at x=3: the way out into the wilds)
const char* YARD = R"({
  "version": 1,
  "sheets": [ {"path": "art/penguin.png", "cols": 16, "rows": 1},
              {"path": "art/terrain.png", "cols": 8, "rows": 4},
              {"path": "art/font.png", "cols": 16, "rows": 6} ],
  "labelStyle": {"sheet": 2, "height": 0.2, "advance": 0.1, "drop": 0.02},
  "cameraRig": {"view": "topdown"},
  "sky": {"zenith": [0, 0, 1]},
  "archetypes": {
    "hero":  {"scale": [0.9, 0.9], "offset": [0, 0.32], "controlled": true, "speed": 3.0,
              "radius": 0.3, "autoAnim": true, "anim": "idle", "label": "HERO",
              "clips": {"idle": {"first": 7, "count": 1, "fps": 0},
                        "walk": {"first": 0, "count": 5, "fps": 10}}},
    "enemy": {"scale": [0.9, 0.9], "offset": [0, 0.32], "label": "ENEMY",
              "patrol": {"delta": [2, 0, 0], "speed": 1.0}},
    "shim":  {"scale": [0.9, 0.9], "offset": [0, 0.32], "tint": [1, 1, 1, 0.3],
              "patrol": {"delta": [2, 0, 0], "speed": 1.0, "phase": 1.0}}
  },
  "terrain": {
    "sheet": 1, "seed": 42, "radius": 6, "origin": [0, 0],
    "wild": {"scale": 8, "biomes": [
      {"name": "dunes",  "upTo": 128, "floor": [5, 6],
       "props": [{"cell": 21, "perMille": 200, "scale": [1, 1], "solid": true, "radius": 0.25}]},
      {"name": "meadow", "upTo": 256, "floor": [0, 1, 2],
       "props": [{"cell": 16, "perMille": 300, "scale": [0.5, 0.5]}]} ]},
    "legend": {
      "#": {"wall": 9, "top": 10, "height": 2},
      ".": {"floor": 8},
      "~": {"floor": 13, "floorFrames": 2, "floorFps": 1.5, "solid": true},
      "H": {"floor": 8, "spawn": "hero"},
      "E": {"floor": 8, "spawn": ["enemy", "shim"]}
    },
    "rows": [ "#######",
              "#H....#",
              "#..~..#",
              "#E....#",
              "###.###" ]
  },
  "controls": {"bindings": {"w": "move_north"}}
})";

Vec3 posOf(const SimState& st, EntityId id) {
    return motionAt(st.entities.at(id), static_cast<float>(st.clock.time()));
}

void step(const Scene& scene, SimState& st, const char* held = nullptr,
          const char* pressed = nullptr, Diff* outDiff = nullptr) {
    ResolvedActions a;
    if (held) a.down.insert(held);
    if (pressed) { a.pressed.insert(pressed); a.down.insert(pressed); }
    Diff diff;
    stepSim(scene, st, a, outDiff ? *outDiff : diff);
}

bool near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }

// --- terrain ------------------------------------------------------------------

void test_terrain_authored() {
    Scene scene;
    std::string err;
    CHECK_MSG(loadFromString(YARD, scene, err), err.c_str());
    const Terrain& t = scene.terrain;
    CHECK(t.enabled);
    CHECK(t.width == 7 && t.height == 5);

    // Walls, water and floor are what the map says they are.
    CHECK(t.wallHeightAt(0, 0) == 2);
    CHECK(t.describe(0, 0).solid);
    CHECK(t.wallHeightAt(2, 1) == 0);
    CHECK(!t.describe(2, 1).solid);
    CHECK(t.describe(2, 1).floor == 8);
    CHECK(t.describe(3, 2).solid);               // water blocks, without a wall
    CHECK(t.wallHeightAt(3, 2) == 0);
    CHECK(t.describe(3, 2).floorCount == 2);
    CHECK(!t.describe(3, 4).solid);              // the gap in the south wall

    // Spawn cells became entities standing at the cell centre.
    EntityId hero = scene.idOf("hero@1,1");
    CHECK(hero != 0);
    CHECK(scene.rigFollow == hero);              // rig defaults to the controlled entity
    const Instance& h = scene.initialState.instances.at(hero);
    CHECK(near(h.pos.x, 1.5f) && near(h.pos.z, 1.5f) && near(h.pos.y, 0.0f));
    CHECK(near(h.offset.y, 0.32f));
    CHECK(scene.idOf("enemy@1,3") != 0);
    CHECK(scene.idOf("shim@1,3") != 0);          // one cell, two archetypes
    CHECK(scene.initialState.sky.enabled);

    // Terrain tiles are not entities: only the 3 spawns and their label glyphs.
    CHECK(scene.initialState.instances.size() == 3 + 4 + 5);   // HERO + ENEMY glyphs
}

void test_terrain_errors() {
    Scene scene;
    std::string err;
    CHECK(!loadFromString(R"({"version": 1, "terrain": {
        "legend": {"#": {"wall": 1}}, "rows": ["##", "#?"] }})", scene, err));
    CHECK(err.find("terrain.rows[1]") != std::string::npos);
    CHECK(err.find("'?'") != std::string::npos);

    Scene scene2;
    CHECK(!loadFromString(R"({"version": 1, "terrain": {
        "legend": {"#": {"wall": 1}}, "rows": ["###", "##"] }})", scene2, err));
    CHECK(err.find("same length") != std::string::npos);

    Scene scene3;
    CHECK(!loadFromString(R"({"version": 1, "terrain": {
        "legend": {"X": {"spawn": "nobody"}}, "rows": ["X"] }})", scene3, err));
    CHECK(err.find("unknown archetype 'nobody'") != std::string::npos);
}

// The heart of the procedural wilds: a cell is a pure function of its
// coordinates and the seed. Leave, come back, rebuild it — same cell.
void test_terrain_procedural() {
    Scene scene;
    std::string err;
    CHECK_MSG(loadFromString(YARD, scene, err), err.c_str());
    const Terrain& t = scene.terrain;

    // Every wild cell exists, far beyond the authored map, negatives included.
    CHECK(t.describe(1000, -2000).exists);
    CHECK(t.describe(-31, 17).exists);

    // Rebuilding a cell yields byte-identical instances.
    int props = 0, solidProps = 0, dunes = 0, meadow = 0;
    for (int z = -40; z < 40; ++z) {
        for (int x = -40; x < 40; ++x) {
            std::vector<Instance> a, b;
            t.buildCell(x, z, a);
            t.buildCell(x, z, b);
            CHECK(a.size() == b.size());
            if (a.size() == b.size() && !a.empty())
                CHECK(std::memcmp(a.data(), b.data(), a.size() * sizeof(Instance)) == 0);
            CellDesc d = t.describe(x, z);
            if (d.hasProp) { ++props; if (d.propSolid) ++solidProps; }
            if (d.floor == 5 || d.floor == 6) ++dunes;
            if (d.floor <= 2) ++meadow;
            if (d.hasProp) {   // a plant stands inside its own cell
                CHECK(d.propX > x && d.propX < x + 1);
                CHECK(d.propZ > z && d.propZ < z + 1);
            }
        }
    }
    // Both biomes and both kinds of plant actually occur.
    CHECK(dunes > 200 && meadow > 200);
    CHECK(props > 300 && solidProps > 50);

    // A different seed grows a different world.
    Terrain other = t;
    other.seed = 43;
    int differing = 0;
    for (int i = 0; i < 400; ++i) {
        CellDesc a = t.describe(100 + i, 50), b = other.describe(100 + i, 50);
        if (a.floor != b.floor || a.hasProp != b.hasProp) ++differing;
    }
    CHECK(differing > 100);

    // Authored cells never grow plants; walls and water stay clear.
    for (int z = 0; z < 5; ++z)
        for (int x = 0; x < 7; ++x)
            CHECK(!t.describe(x, z).hasProp);
}

void test_terrain_streamer() {
    Scene scene;
    std::string err;
    CHECK_MSG(loadFromString(YARD, scene, err), err.c_str());
    const Terrain& t = scene.terrain;

    TerrainStreamer streamer;
    Diff first;
    streamer.update(t, {100.5f, 0, 100.5f}, first);
    const size_t cells = streamer.loadedCells();
    CHECK(cells > 100);                   // a disc of radius 6
    CHECK(first.removals.empty());
    CHECK(!first.upserts.empty());
    for (const auto& up : first.upserts) CHECK(up.first >= TERRAIN_ID_BASE);

    // Standing still inside the cell costs nothing.
    Diff still;
    streamer.update(t, {100.9f, 0, 100.1f}, still);
    CHECK(still.upserts.empty() && still.removals.empty());

    // Remember what cell (100, 100) looked like, walk far away, come back.
    std::vector<Instance> before;
    t.buildCell(100, 100, before);
    Diff away;
    streamer.update(t, {300.5f, 0, 300.5f}, away);
    CHECK(streamer.loadedCells() == cells);
    CHECK(away.removals.size() == first.upserts.size());
    // No id is both removed and re-used within one diff (upserts apply first).
    std::set<EntityId> removed(away.removals.begin(), away.removals.end());
    for (const auto& up : away.upserts) CHECK(removed.count(up.first) == 0);

    Diff back;
    streamer.update(t, {100.5f, 0, 100.5f}, back);
    CHECK(streamer.loadedCells() == cells);
    CHECK(back.upserts.size() == first.upserts.size());
    std::vector<Instance> after;
    t.buildCell(100, 100, after);
    CHECK(before.size() == after.size());
    CHECK(!before.empty() &&
          std::memcmp(before.data(), after.data(), before.size() * sizeof(Instance)) == 0);
}

// --- collision ----------------------------------------------------------------

void test_collision() {
    Scene scene;
    std::string err;
    CHECK_MSG(loadFromString(YARD, scene, err), err.c_str());
    EntityId hero = scene.idOf("hero@1,1");
    SimState st = makeSimState(scene);

    // Walk west into the wall at x < 1: stopped one radius short of it.
    for (int i = 0; i < 120; ++i) step(scene, st, "move_west");
    Vec3 p = posOf(st, hero);
    CHECK(p.x >= 1.3f - 0.02f && p.x < 1.4f);
    CHECK(near(p.z, 1.5f));

    // Pushing diagonally into the wall slides along it instead of sticking.
    ResolvedActions diag;
    diag.down.insert("move_west");
    diag.down.insert("move_south");
    for (int i = 0; i < 20; ++i) { Diff d; stepSim(scene, st, diag, d); }
    Vec3 q = posOf(st, hero);
    CHECK(q.z > p.z + 0.4f);                    // moved south along the wall
    CHECK(q.x >= 1.3f - 0.02f && q.x < 1.4f);   // still held off the wall

    // The pond in the middle blocks too: walk east along z=2.5 from x≈1.3.
    for (int i = 0; i < 200; ++i) {
        Vec3 at = posOf(st, hero);
        ResolvedActions a;
        if (at.z < 2.45f) a.down.insert("move_south");
        else if (at.z > 2.55f) a.down.insert("move_north");
        else a.down.insert("move_east");
        Diff d; stepSim(scene, st, a, d);
    }
    CHECK(posOf(st, hero).x < 3.0f - 0.3f + 0.02f);   // stopped at the water's edge

    // Out through the gap in the south wall and into the wilds.
    CHECK(!scene.terrain.blocksCircle(3.5f, 4.5f, 0.3f));
    CHECK(scene.terrain.blocksCircle(2.5f, 4.5f, 0.3f));
}

// --- camera rig ---------------------------------------------------------------

void test_rig() {
    Scene scene;
    std::string err;
    CHECK_MSG(loadFromString(YARD, scene, err), err.c_str());
    EntityId hero = scene.idOf("hero@1,1");
    SimState st = makeSimState(scene);
    CHECK(st.rig.view == RIG_TOPDOWN && st.rig.quarter == 0);

    // Facing north, "north" walks toward -Z.
    Vec3 p0 = posOf(st, hero);
    for (int i = 0; i < 6; ++i) step(scene, st, "move_south");
    Vec3 p1 = posOf(st, hero);
    CHECK(p1.z > p0.z + 0.2f && near(p1.x, p0.x));

    // Turn the camera right (now facing east): "north" = forward = +X.
    step(scene, st, nullptr, "camera_right");
    CHECK(st.rig.quarter == 1);
    for (int i = 0; i < 12; ++i) step(scene, st, "move_north");
    Vec3 p2 = posOf(st, hero);
    CHECK(p2.x > p1.x + 0.4f && near(p2.z, p1.z));

    // The turn is eased, never a snap: partway there mid-turn, exact at the end.
    RigState rig = makeRigState(scene.rig);
    rigTurn(rig, +1, 10.0);
    CHECK(near(rigYaw(rig, 10.0), 0.0f));
    float mid = rigYaw(rig, 10.0 + RIG_TURN_SECONDS * 0.5);
    CHECK(mid > 0.3f && mid < RIG_HALF_PI - 0.3f);
    CHECK(near(rigYaw(rig, 10.0 + RIG_TURN_SECONDS), RIG_HALF_PI));
    CHECK(near(rigYaw(rig, 99.0), RIG_HALF_PI));
    // A second turn started mid-way continues from where the camera is.
    rigTurn(rig, +1, 10.0 + RIG_TURN_SECONDS * 0.5);
    CHECK(near(rigYaw(rig, 10.0 + RIG_TURN_SECONDS * 0.5), mid));
    CHECK(rig.quarter == 2);

    // View switch: an eased blend from top-down (0) to dungeon (1).
    step(scene, st, nullptr, "camera_view");
    CHECK(st.rig.view == RIG_DUNGEON);
    double now = st.clock.time();
    CHECK(rigBlend(st.rig, now + RIG_VIEW_SECONDS) == 1.0f);
    float half = rigBlend(st.rig, now + RIG_VIEW_SECONDS * 0.5);
    CHECK(half > 0.2f && half < 0.8f);

    // The camera sits behind the focus and looks at it, in both views.
    RigConfig cfg;
    RigState top = makeRigState(cfg);
    RigPose pose = rigPose(cfg, top, 0.0, {5, 0, 5});
    Camera cam = rigCamera(pose, 1.0f);
    CHECK(near(cam.position.x, 5.0f) && cam.position.z > 5.0f && cam.position.y > 3.0f);
    CHECK(near(cam.target.x, 5.0f) && near(cam.target.z, 5.0f));
    // Pulled in, it comes closer to the pivot (and never past the minimum).
    Camera close = rigCamera(pose, 0.5f);
    CHECK(close.position.z < cam.position.z && close.position.z > 5.0f);
    Camera tooClose = rigCamera(pose, 0.0f);
    CHECK(tooClose.position.z > 5.0f);

    // A wall on the line of sight shortens the clear stretch; open ground doesn't.
    const Terrain& t = scene.terrain;
    CHECK(t.sightClearance({1.5f, 0.8f, 1.5f}, {4.5f, 0.8f, 1.5f}) == 1.0f);
    float blockedLine = t.sightClearance({1.5f, 0.8f, 1.5f}, {1.5f, 1.2f, -2.5f});
    CHECK(blockedLine < 0.2f);
    // ... but an eye high above the walls sees over them.
    CHECK(t.sightClearance({1.5f, 0.8f, 1.5f}, {1.5f, 9.0f, -0.6f}) == 1.0f);
}

// --- labels, auto animation, patrols -------------------------------------------

void test_labels_and_anim() {
    Scene scene;
    std::string err;
    CHECK_MSG(loadFromString(YARD, scene, err), err.c_str());
    EntityId hero = scene.idOf("hero@1,1");
    SimState st = makeSimState(scene);

    auto lab = scene.labels.find(hero);
    CHECK(lab != scene.labels.end() && lab->second.size() == 4);   // H-E-R-O
    const std::vector<EntityId>& glyphs = lab->second;
    for (EntityId g : glyphs) {
        const Instance& gi = st.entities.at(g);
        CHECK(gi.billboard == BILLBOARD_GROUNDED);
        CHECK(gi.offset.y < 0.0f);          // hangs below the feet
        CHECK(gi.sheet == 2.0f);
    }
    // Glyphs spread left to right around the anchor.
    CHECK(st.entities.at(glyphs[0]).offset.x < 0.0f);
    CHECK(st.entities.at(glyphs[3]).offset.x > 0.0f);

    // Starting to walk: walk clip, and the label takes the hero's motion.
    step(scene, st);   // (the first step also syncs every patrol — get it out of the way)
    Diff d;
    step(scene, st, "move_east", nullptr, &d);
    const Instance& h = st.entities.at(hero);
    CHECK(h.anim.y == 5.0f);                // walk: 5 frames
    CHECK(h.scale.x > 0.0f);                // heading screen-right: not mirrored
    for (EntityId g : glyphs) {
        const Instance& gi = st.entities.at(g);
        CHECK(gi.vel.x == h.vel.x && gi.motionStart == h.motionStart);
        CHECK(gi.pos.x == h.pos.x && gi.pos.z == h.pos.z);
    }
    CHECK(d.upserts.size() == 1 + glyphs.size());

    // Heading screen-left mirrors the sprite; stopping returns to idle.
    step(scene, st, "move_west");
    CHECK(st.entities.at(hero).scale.x < 0.0f);
    step(scene, st);
    CHECK(st.entities.at(hero).anim.y == 1.0f);
    CHECK(st.entities.at(hero).anim.x == 7.0f);
    CHECK(st.entities.at(hero).scale.x < 0.0f);   // keeps facing where it went

    // Removing an entity takes its label with it.
    Diff rm;
    removeEntity(scene, st, hero, rm);
    CHECK(rm.removals.size() == 1 + glyphs.size());
    CHECK(st.entities.count(hero) == 0 && st.entities.count(glyphs[0]) == 0);
}

void test_patrol() {
    Scene scene;
    std::string err;
    CHECK_MSG(loadFromString(YARD, scene, err), err.c_str());
    EntityId enemy = scene.idOf("enemy@1,3");
    EntityId shim  = scene.idOf("shim@1,3");
    CHECK(scene.patrolIds.size() == 2);
    const Patrol& pat = scene.attrs.at(enemy).patrol;
    CHECK(pat.legTicks == 120);             // 2 units at 1 unit/s, 60 Hz
    CHECK(scene.attrs.at(shim).patrol.phaseTicks == 60);

    SimState st = makeSimState(scene);
    // Out for 2 seconds, back for 2 seconds, forever; never past either end.
    for (int tick = 0; tick <= 600; ++tick) {
        Vec3 e = posOf(st, enemy);
        CHECK(e.x >= 1.5f - 1e-3f && e.x <= 3.5f + 1e-3f);
        if (tick == 120) CHECK(near(e.x, 3.5f, 2e-3f));
        if (tick == 240) CHECK(near(e.x, 1.5f, 2e-3f));
        if (tick == 60)  CHECK(near(e.x, 2.5f, 2e-3f));
        // The shim is the same patrol one second ahead — a real lookahead.
        if (tick % 37 == 0 && tick + 60 <= 600) {
            SimState future = st;
            for (int k = 0; k < 60; ++k) step(scene, future);
            CHECK(near(posOf(st, shim).x, posOf(future, enemy).x, 2e-3f));
        }
        step(scene, st);
    }

    // The enemy's label walks with it.
    const std::vector<EntityId>& glyphs = scene.labels.at(enemy);
    CHECK(glyphs.size() == 5);
    Vec3 e = posOf(st, enemy), g = posOf(st, glyphs[0]);
    CHECK(near(e.x, g.x) && near(e.z, g.z));

    // Starting the sim at an arbitrary tick still puts the patrol where it
    // belongs (the first step re-syncs every patrol to the clock).
    SimState late = makeSimState(scene);
    late.clock.tick = 150;                  // 30 ticks into the way back
    step(scene, late);
    CHECK(near(posOf(late, enemy).x, 3.5f - 31.0f / 60.0f, 2e-3f));
}

// --- the real demo scene --------------------------------------------------------

void test_explore_scene() {
    Scene scene;
    std::string err;
    bool ok = loadScene("src/levels/explore.json", scene, err);
    CHECK_MSG(ok, err.c_str());
    if (!ok) return;
    CHECK(scene.terrain.enabled && scene.terrain.wild);
    CHECK(scene.rig.enabled && scene.rigFollow != 0);
    CHECK(scene.initialState.sky.enabled);
    CHECK(scene.attrs.at(scene.rigFollow).controlled);

    // Nothing spawns inside something solid.
    for (const auto& kv : scene.attrs) {
        if (scene.labels.count(kv.first) == 0 && !kv.second.patrol.enabled) continue;
        const Instance& inst = scene.initialState.instances.at(kv.first);
        CHECK(!scene.terrain.describe(static_cast<int>(std::floor(inst.pos.x)),
                                      static_cast<int>(std::floor(inst.pos.z))).solid);
    }
    // Patrols stay on walkable ground along their whole leg.
    for (EntityId id : scene.patrolIds) {
        const Patrol& p = scene.attrs.at(id).patrol;
        for (int i = 0; i <= 20; ++i) {
            Vec3 at = p.a + (p.b - p.a) * (i / 20.0f);
            CHECK(!scene.terrain.blocksCircle(at.x, at.z, 0.2f));
        }
    }

    // Determinism: the same scripted walk (with camera turns and a view
    // switch) played twice ends in the identical state.
    auto play = [&](SimState& st) {
        for (int tick = 0; tick < 900; ++tick) {
            ResolvedActions a;
            int phase = (tick / 90) % 4;
            a.down.insert(phase == 0 ? "move_north" : phase == 1 ? "move_east"
                        : phase == 2 ? "move_south" : "move_west");
            if (tick % 200 == 50)  a.pressed.insert("camera_right");
            if (tick == 400)       a.pressed.insert("camera_view");
            Diff d;
            stepSim(scene, st, a, d);
        }
    };
    SimState a = makeSimState(scene), b = makeSimState(scene);
    play(a);
    play(b);
    CHECK(a.entities.size() == b.entities.size());
    bool same = true;
    for (const auto& kv : a.entities) {
        auto it = b.entities.find(kv.first);
        if (it == b.entities.end() ||
            std::memcmp(&kv.second, &it->second, sizeof(Instance)) != 0) same = false;
    }
    CHECK(same);
    CHECK(a.rig.quarter == b.rig.quarter && a.rig.view == b.rig.view);
    CHECK(a.rig.view == RIG_DUNGEON);

    // The hero never ends up inside a wall, whatever it walked into.
    Vec3 hero = posOf(a, scene.rigFollow);
    CHECK(!scene.terrain.blocksCircle(hero.x, hero.z, 0.2f));
}

} // namespace

void test_world() {
    test_terrain_authored();
    test_terrain_errors();
    test_terrain_procedural();
    test_terrain_streamer();
    test_collision();
    test_rig();
    test_labels_and_anim();
    test_patrol();
    test_explore_scene();
}
