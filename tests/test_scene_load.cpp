// test_scene_load.cpp — every canonical scene file must load successfully.
// Run from the repo root (make test-unit does).
#include "test_harness.h"

#include "scene.h"

#include <filesystem>
#include <string>

using namespace cv;
namespace fs = std::filesystem;

void test_scene_load() {
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
