// renderer.h — turns world state into one instanced draw call.
//
// The renderer owns all GL objects: a shared unit-quad, the per-instance VBO,
// the shader program, and one GL_TEXTURE_2D_ARRAY holding every sprite sheet
// (one sheet per layer — see texture.h). It keeps a packed array of instances
// plus an id->slot map so diffs can upsert/remove individual entities without
// rebuilding everything.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "camera.h"
#include "gl.h"
#include "instance.h"
#include "texture.h"
#include "world_state.h"

namespace cv {

class Renderer {
public:
    // Sprite-sheet capacity of the shared texture array. Every loaded sheet
    // occupies one square layer; cells are repacked to fit (texture.h).
    static const int MAX_SHEETS = 16;
    static const int SHEET_LAYER_SIZE = 1024;

    // Creates GL objects. Sheets are loaded separately via loadSheet().
    bool init();
    void shutdown();

    // Loads a sprite sheet (a cols×rows grid of equal cells) into the next free
    // layer of the shared texture array and returns its sheet index — the value
    // Instance::sheet selects. Callable any time after init(), including between
    // scenes. Idempotent: re-loading the same (path, cols, rows) returns the
    // existing index instead of burning a layer (keeps scene hot-reload cheap).
    // Returns -1 on failure.
    int loadSheet(const char* path, int cols, int rows);

    // Full replace: rebuild the instance set and camera list from scratch.
    void applyState(const WorldState& state);

    // Incremental change: upsert/remove instances, optionally edit cameras.
    void applyDiff(const Diff& diff);

    // Draw every instance in one call, evaluated at the given time (seconds).
    void render(float time);

    void setViewport(int w, int h);

    Camera* activeCamera();

private:
    void uploadInstances();              // re-push the packed buffer to the GPU
    void upsert(EntityId id, const Instance& inst);
    void remove(EntityId id);

    GLuint program_ = 0;
    GLuint vao_ = 0;
    GLuint quadVbo_ = 0;
    GLuint quadEbo_ = 0;
    GLuint instanceVbo_ = 0;

    // One array texture shared by every sheet; per-sheet grid info feeds the
    // uSheetGrid uniform array so the shader can do per-instance UV math.
    TextureArray textureArray_;
    struct LoadedSheet {
        std::string path;
        int cols = 1;
        int rows = 1;
        SheetGrid grid;
    };
    std::vector<LoadedSheet> sheets_;

    // Packed instance storage + id -> index into `instances_`.
    std::vector<Instance> instances_;
    std::unordered_map<EntityId, size_t> idToSlot_;
    std::vector<EntityId> slotToId_;
    bool instancesDirty_ = false;

    std::vector<Camera> cameras_;
    int activeCamera_ = 0;

    int viewportW_ = 1280;
    int viewportH_ = 720;
};

} // namespace cv
