// test_scene_load.cpp — every canonical scene file must load successfully.
// Run from the repo root (make test-unit does).
#include "test_harness.h"

#include "scene.h"

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

void test_scene_load() {
    test_scene_sheets();
    test_scene_orientation();
    test_scene_conditions();
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
