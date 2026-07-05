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
            { "id": "wall", "pos": [1, 0, 0], "billboard": false, "rotation": 1.0 },
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
    // Billboards ignore orientation fields entirely.
    CHECK(scene.initialState.instances[scene.idOf("sprite")].pitch == 0.0f);
}

void test_scene_load() {
    test_scene_sheets();
    test_scene_orientation();
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
