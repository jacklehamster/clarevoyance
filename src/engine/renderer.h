// renderer.h — turns world state into two instanced draw calls.
//
// The renderer owns all GL objects: a shared unit-quad, per-bucket instance
// VBOs, the shader program, and one GL_TEXTURE_2D_ARRAY holding every sprite
// sheet (one sheet per layer — see texture.h).
//
// Instances are partitioned into two buckets by tint alpha: OPAQUE (drawn
// first, depth write on) and TRANSLUCENT (drawn second, depth test on but
// write off, for shims). Each bucket is a packed array + its own VAO/VBO —
// the WebGL2-safe way to draw two instance ranges (no baseInstance in ES 3.0).
// A shared id->slot map lets diffs upsert/remove individual entities without
// rebuilding everything; an upsert that changes an instance's opacity class
// migrates it between buckets automatically.
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

    // Draw every instance (opaque pass, then translucent pass), evaluated at
    // the given time (seconds).
    void render(float time);

    void setViewport(int w, int h);

    Camera* activeCamera();

private:
    // Bucket indices + the alpha threshold that separates them.
    static const int OPAQUE = 0;
    static const int TRANSLUCENT = 1;
    static const int BUCKET_COUNT = 2;
    static constexpr float OPAQUE_ALPHA_MIN = 0.999f;

    // One packed instance array + VAO/VBO per bucket (shared quad geometry).
    struct Bucket {
        GLuint vao = 0;
        GLuint vbo = 0;
        std::vector<Instance> instances;
        std::vector<EntityId> slotToId;
        bool dirty = false;
    };

    static int bucketFor(const Instance& inst) {
        return inst.tint.w < OPAQUE_ALPHA_MIN ? TRANSLUCENT : OPAQUE;
    }

    void uploadBucket(Bucket& b);        // re-push a packed buffer to the GPU
    void upsert(EntityId id, const Instance& inst);
    void remove(EntityId id);
    void insertInto(int bucket, EntityId id, const Instance& inst);
    void removeFromBucket(int bucket, size_t slot);

    GLuint program_ = 0;
    GLuint quadVbo_ = 0;
    GLuint quadEbo_ = 0;

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

    // Instance storage: id -> (bucket, slot within that bucket's packed array).
    Bucket buckets_[BUCKET_COUNT];
    struct SlotRef {
        int bucket = OPAQUE;
        size_t index = 0;
    };
    std::unordered_map<EntityId, SlotRef> idToSlot_;

    std::vector<Camera> cameras_;
    int activeCamera_ = 0;

    int viewportW_ = 1280;
    int viewportH_ = 720;
};

} // namespace cv
