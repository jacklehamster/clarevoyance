// test_scene_load.cpp — every canonical scene file must load successfully.
// Run from the repo root (make test-unit does).
#include "test_harness.h"

#include "scene.h"
#include "sim.h"

#include <cstdio>
#include <filesystem>
#include <string>

using namespace cv;
namespace fs = std::filesystem;

// Write `json` to a temp file, load it as a Scene, return success.
static bool loadFromString(const std::string& json, Scene& out, std::string& err) {
    fs::create_directories("build");
    const char* path = "build/test_scene_tmp.json";
    std::FILE* f = std::fopen(path, "wb");
    if (!f) { err = "cannot open temp file"; return false; }
    std::fwrite(json.data(), 1, json.size(), f);
    std::fclose(f);
    bool ok = loadScene(path, out, err);
    fs::remove(path);
    return ok;
}

// Multi-sheet parsing: "sheets" array, per-entity sheet index, and the
// backward-compatible singular "sheet" object mapping to sheets[0].
static void test_scene_sheets() {
    {
        Scene scene;
        std::string err;
        bool ok = loadFromString(R"({
            "version": 1,
            "sheets": [
                { "path": "art/penguin.png", "cols": 16, "rows": 1 },
                { "path": "art/tiles.png",   "cols": 2,  "rows": 1 }
            ],
            "entities": [
                { "id": "a", "pos": [0, 0, 0] },
                { "id": "b", "pos": [1, 0, 0], "sheet": 1 }
            ]
        })", scene, err);
        CHECK_MSG(ok, err.c_str());
        CHECK(scene.sheets.size() == 2);
        CHECK(scene.sheets[1].path == "art/tiles.png");
        CHECK(scene.sheets[1].cols == 2);
        CHECK(scene.initialState.instances[scene.idOf("a")].sheet == 0.0f);
        CHECK(scene.initialState.instances[scene.idOf("b")].sheet == 1.0f);
    }
    {
        // Old singular "sheet" still accepted as sheets[0].
        Scene scene;
        std::string err;
        bool ok = loadFromString(R"({
            "version": 1,
            "sheet": { "path": "art/penguin.png", "cols": 16, "rows": 1 },
            "entities": [ { "id": "a", "pos": [0, 0, 0] } ]
        })", scene, err);
        CHECK_MSG(ok, err.c_str());
        CHECK(scene.sheets.size() == 1);
        CHECK(scene.sheets[0].cols == 16);
    }
}

// Orientation parsing: non-billboard entities take yaw ("rotation") and
// "pitch" in radians; the basis itself is built in the vertex shader.
static void test_scene_orientation() {
    Scene scene;
    std::string err;
    bool ok = loadFromString(R"({
        "version": 1,
        "entities": [
            { "id": "floor", "pos": [0, 0, 0], "billboard": false,
              "rotation": 0.5, "pitch": -1.5707963 },
            { "id": "wall", "pos": [1, 0, 0], "billboard": false, "rotation": 1.0,
              "tint": [1, 1, 1, 0.4] },
            { "id": "sprite", "pos": [2, 0, 0], "billboard": true, "pitch": 2.0 }
        ]
    })", scene, err);
    CHECK_MSG(ok, err.c_str());
    const Instance& floor = scene.initialState.instances[scene.idOf("floor")];
    CHECK(floor.rotation == 0.5f);
    CHECK(floor.pitch < -1.57f && floor.pitch > -1.58f);
    const Instance& wall = scene.initialState.instances[scene.idOf("wall")];
    CHECK(wall.rotation == 1.0f);
    CHECK(wall.pitch == 0.0f);      // defaults upright
    CHECK(wall.tint.w == 0.4f);     // translucent tint parsed
    CHECK(floor.tint.w == 1.0f);    // tint defaults opaque
    // Billboards ignore orientation fields entirely.
    CHECK(scene.initialState.instances[scene.idOf("sprite")].pitch == 0.0f);
}

// Condition parsing: comparison ops, all/any composition, and numeric/boolean
// values for set_flag / add_flag.
static void test_scene_conditions() {
    Scene scene;
    std::string err;
    bool ok = loadFromString(R"({
        "version": 1,
        "entities": [ { "id": "a", "pos": [0, 0, 0] } ],
        "events": [
            {
                "trigger": { "type": "start" },
                "condition": { "all": [
                    { "flag": "keys", "op": "ge", "value": 3 },
                    { "any": [
                        { "flag": "door_open", "value": true },
                        { "flag": "lives", "op": "ne", "value": 0 }
                    ] }
                ] },
                "actions": [
                    { "type": "add_flag", "flag": "keys", "value": 1 },
                    { "type": "set_flag", "flag": "score", "value": 250 },
                    { "type": "set_flag", "flag": "seen", "value": true }
                ]
            }
        ]
    })", scene, err);
    CHECK_MSG(ok, err.c_str());
    CHECK(scene.events.size() == 1);
    if (scene.events.size() != 1) return;
    const Condition& c = scene.events[0].condition;
    CHECK(c.kind == Condition::Kind::All && c.children.size() == 2);
    if (c.children.size() == 2) {
        CHECK(c.children[0].kind == Condition::Kind::Flag);
        CHECK(c.children[0].flag == "keys");
        CHECK(c.children[0].op == Condition::Op::Ge);
        CHECK(c.children[0].value == 3.0);
        const Condition& any = c.children[1];
        CHECK(any.kind == Condition::Kind::Any && any.children.size() == 2);
        if (any.children.size() == 2) {
            CHECK(any.children[0].value == 1.0);   // true → 1.0
            CHECK(any.children[0].op == Condition::Op::Eq);
            CHECK(any.children[1].op == Condition::Op::Ne);
            CHECK(any.children[1].value == 0.0);
        }
    }
    const std::vector<Action>& acts = scene.events[0].actions;
    CHECK(acts.size() == 3);
    if (acts.size() == 3) {
        CHECK(acts[0].type == Action::Type::AddFlag && acts[0].value == 1.0);
        CHECK(acts[1].type == Action::Type::SetFlag && acts[1].value == 250.0);
        CHECK(acts[2].type == Action::Type::SetFlag && acts[2].value == 1.0);
    }
}

// Archetypes: template fields apply as defaults, entity fields override, and
// named clips resolve for both string "anim" fields and set_anim actions.
static void test_scene_archetypes() {
    Scene scene;
    std::string err;
    bool ok = loadFromString(R"({
        "version": 1,
        "archetypes": {
            "penguin": {
                "scale": [0.9, 0.9], "billboard": true, "sheet": 0, "speed": 2.5,
                "clips": {
                    "idle":      { "first": 0, "count": 1, "fps": 0 },
                    "walk":      { "first": 0, "count": 5, "fps": 10 },
                    "surprised": { "first": 6, "count": 1, "fps": 0 }
                }
            }
        },
        "entities": [
            { "id": "a", "archetype": "penguin", "pos": [1, 0, 0], "anim": "walk" },
            { "id": "b", "archetype": "penguin", "pos": [2, 0, 0],
              "scale": [2, 2], "speed": 4.0,
              "anim": { "first": 3, "count": 2, "fps": 5 } },
            { "id": "plain", "pos": [3, 0, 0] }
        ],
        "events": [
            { "trigger": { "type": "start" },
              "actions": [ { "type": "set_anim", "entity": "a", "clip": "surprised" } ] }
        ]
    })", scene, err);
    CHECK_MSG(ok, err.c_str());

    // Archetype parsed onto the scene, entities mapped to it.
    CHECK(scene.archetypes.count("penguin") == 1);
    CHECK(scene.archetypes["penguin"].clips.size() == 3);
    CHECK(scene.archetypeOf.count(scene.idOf("a")) == 1);
    CHECK(scene.archetypeOf.count(scene.idOf("plain")) == 0);

    // Defaults from the archetype...
    const Instance& a = scene.initialState.instances[scene.idOf("a")];
    CHECK(a.scale.x == 0.9f && a.scale.y == 0.9f);
    CHECK(a.billboard == 1.0f);
    CHECK(scene.attrs[scene.idOf("a")].speed == 2.5f);
    // ...string anim resolves the archetype clip
    CHECK(a.anim.x == 0.0f && a.anim.y == 5.0f && a.anim.z == 10.0f);

    // Entity fields override archetype defaults (scale, speed, inline anim).
    const Instance& b = scene.initialState.instances[scene.idOf("b")];
    CHECK(b.scale.x == 2.0f && b.scale.y == 2.0f);
    CHECK(b.billboard == 1.0f);                        // still from the archetype
    CHECK(scene.attrs[scene.idOf("b")].speed == 4.0f);
    CHECK(b.anim.x == 3.0f && b.anim.y == 2.0f && b.anim.z == 5.0f);

    // set_anim with "clip" resolves through the target's archetype at runtime.
    CHECK(scene.events.size() == 1 && scene.events[0].actions.size() == 1);
    CHECK(scene.events[0].actions[0].clip == "surprised");
    SimState st = makeSimState(scene);
    Diff diff;
    stepSim(scene, st, ResolvedActions{}, diff);
    const Instance& aAfter = st.entities[scene.idOf("a")];
    CHECK(aAfter.anim.x == 6.0f && aAfter.anim.y == 1.0f && aAfter.anim.z == 0.0f);

    // clipOf helper: hit and miss.
    const Clip* walk = scene.clipOf(scene.idOf("a"), "walk");
    CHECK(walk && walk->first == 0 && walk->count == 5 && walk->fps == 10.0f);
    CHECK(scene.clipOf(scene.idOf("plain"), "walk") == nullptr);
    CHECK(scene.clipOf(scene.idOf("a"), "nope") == nullptr);
}

// Strict validation: every error class rejects the scene with a message that
// carries the offending string and its context path.
static void test_scene_strict() {
    // load must FAIL and the error must contain `expect`.
    auto loadFails = [](const std::string& json, const char* expect) {
        Scene scene;
        std::string err;
        bool ok = loadFromString(json, scene, err);
        if (ok) {
            std::printf("  expected failure containing \"%s\" but scene loaded\n", expect);
            return false;
        }
        if (err.find(expect) == std::string::npos) {
            std::printf("  error \"%s\" does not contain \"%s\"\n", err.c_str(), expect);
            return false;
        }
        return true;
    };

    // Unknown trigger type.
    CHECK(loadFails(R"({ "version": 1,
        "entities": [ { "id": "a", "pos": [0,0,0] } ],
        "events": [ { "trigger": { "type": "proximty" } } ] })",
        "events[0].trigger: unknown trigger type 'proximty'"));

    // Unknown action type (with the event/action indices in the context).
    CHECK(loadFails(R"({ "version": 1,
        "entities": [ { "id": "a", "pos": [0,0,0] } ],
        "events": [
            { "trigger": { "type": "start" }, "actions": [ { "type": "dialogue", "id": "x" } ] },
            { "trigger": { "type": "start" },
              "actions": [ { "type": "dialogue", "id": "y" },
                           { "type": "set_flg", "flag": "f" } ] }
        ] })",
        "events[1].actions[1]: unknown action type 'set_flg'"));

    // Unknown condition op.
    CHECK(loadFails(R"({ "version": 1,
        "entities": [ { "id": "a", "pos": [0,0,0] } ],
        "events": [ { "trigger": { "type": "start" },
                      "condition": { "flag": "x", "op": "gte", "value": 1 } } ] })",
        "events[0].condition: unknown condition op 'gte'"));

    // Unknown op nested inside a composition keeps the full path.
    CHECK(loadFails(R"({ "version": 1,
        "entities": [ { "id": "a", "pos": [0,0,0] } ],
        "events": [ { "trigger": { "type": "start" },
                      "condition": { "all": [ { "flag": "x", "op": "max" } ] } } ] })",
        "events[0].condition.all[0]: unknown condition op 'max'"));

    // Duplicate entity ids.
    CHECK(loadFails(R"({ "version": 1,
        "entities": [ { "id": "a", "pos": [0,0,0] },
                      { "id": "a", "pos": [1,0,0] } ] })",
        "entities[1]: duplicate entity id 'a'"));

    // Trigger referencing an entity not defined in the scene.
    CHECK(loadFails(R"({ "version": 1,
        "entities": [ { "id": "a", "pos": [0,0,0] } ],
        "events": [ { "trigger": { "type": "proximity", "entity": "a", "target": "ghost",
                                   "radius": 1 } } ] })",
        "events[0].trigger: 'target' references unknown entity 'ghost'"));

    // Action referencing an entity not defined in the scene.
    CHECK(loadFails(R"({ "version": 1,
        "entities": [ { "id": "a", "pos": [0,0,0] } ],
        "events": [ { "trigger": { "type": "start" },
                      "actions": [ { "type": "remove", "entity": "b" } ] } ] })",
        "events[0].actions[0]: 'entity' references unknown entity 'b'"));

    // Unknown archetype reference.
    CHECK(loadFails(R"({ "version": 1,
        "archetypes": { "penguin": { "scale": [1,1] } },
        "entities": [ { "id": "a", "archetype": "pengiun", "pos": [0,0,0] } ] })",
        "entities[0]: unknown archetype 'pengiun'"));

    // Clip name that the archetype does not define.
    CHECK(loadFails(R"({ "version": 1,
        "archetypes": { "penguin": { "clips": { "walk": { "first": 0, "count": 5, "fps": 10 } } } },
        "entities": [ { "id": "a", "archetype": "penguin", "pos": [0,0,0], "anim": "wlk" } ] })",
        "entities[0].anim: unknown clip 'wlk'"));

    // Clip name on an entity with no archetype.
    CHECK(loadFails(R"({ "version": 1,
        "entities": [ { "id": "a", "pos": [0,0,0], "anim": "walk" } ] })",
        "entities[0].anim: clip name 'walk' requires an archetype"));

    // set_anim clip that does not resolve for the target entity.
    CHECK(loadFails(R"({ "version": 1,
        "archetypes": { "penguin": { "clips": { "walk": { "first": 0, "count": 5, "fps": 10 } } } },
        "entities": [ { "id": "a", "archetype": "penguin", "pos": [0,0,0] } ],
        "events": [ { "trigger": { "type": "start" },
                      "actions": [ { "type": "set_anim", "entity": "a", "clip": "run" } ] } ] })",
        "events[0].actions[0]: clip 'run' does not resolve for entity 'a'"));

    // Malformed vec3 (wrong length) and vec2.
    CHECK(loadFails(R"({ "version": 1,
        "entities": [ { "id": "a", "pos": [0, 0] } ] })",
        "entities[0].pos: expected an array of 3 numbers"));
    CHECK(loadFails(R"({ "version": 1,
        "entities": [ { "id": "a", "pos": [0,0,0], "scale": [1, 1, 1] } ] })",
        "entities[0].scale: expected an array of 2 numbers"));
    CHECK(loadFails(R"({ "version": 1,
        "entities": [ { "id": "a", "pos": [0,0,0], "tint": [1, 1, 1] } ] })",
        "entities[0].tint: expected an array of 4 numbers"));
    CHECK(loadFails(R"({ "version": 1,
        "cameras": [ { "position": [1, "x", 3] } ] })",
        "cameras[0].position: expected an array of 3 numbers"));

    // Version newer than the loader supports.
    CHECK(loadFails(R"({ "version": 2, "entities": [ { "id": "a", "pos": [0,0,0] } ] })",
        "unsupported scene version 2"));

    // Missing version still loads (warns on stderr only).
    {
        Scene scene;
        std::string err;
        bool ok = loadFromString(R"({ "entities": [ { "id": "a", "pos": [0,0,0] } ] })",
                                 scene, err);
        CHECK_MSG(ok, err.c_str());
    }
}

void test_scene_load() {
    test_scene_sheets();
    test_scene_orientation();
    test_scene_conditions();
    test_scene_archetypes();
    test_scene_strict();
    const char* dir = "src/levels";
    CHECK_MSG(fs::is_directory(dir), "run from the repo root");
    if (!fs::is_directory(dir)) return;

    int loaded = 0;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.path().extension() != ".json") continue;

        Scene scene;
        std::string err;
        bool ok = loadScene(entry.path().string().c_str(), scene, err);
        if (!ok)
            std::printf("  scene %s: %s\n", entry.path().c_str(), err.c_str());
        CHECK_MSG(ok, entry.path().c_str());
        CHECK_MSG(!scene.initialState.instances.empty(), entry.path().c_str());
        ++loaded;
    }
    CHECK_MSG(loaded > 0, "no scene files found in src/levels");
}
