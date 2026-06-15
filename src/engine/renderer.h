// renderer.h — turns world state into one instanced draw call.
//
// The renderer owns all GL objects: a shared unit-quad, the per-instance VBO,
// the shader program, and the (single, for now) sprite-sheet texture. It keeps a
// packed array of instances plus an id->slot map so diffs can upsert/remove
// individual entities without rebuilding everything.
#pragma once

#include <cstdint>
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
    // sheetCols/sheetRows describe the grid layout of the one sprite sheet.
    bool init(const char* spriteSheetPath, int sheetCols, int sheetRows);
    void shutdown();

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

    Texture texture_;
    int sheetCols_ = 1;
    int sheetRows_ = 1;

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
