#include "renderer.h"

#include <cmath>
#include <cstddef> // offsetof
#include <SDL.h>

#include "shader.h"

namespace cv {

// ---------------------------------------------------------------------------
// Shaders
// ---------------------------------------------------------------------------

static const char* VERT_SRC = R"glsl(
// Shared quad geometry.
layout(location = 0) in vec2 aCorner;  // unit quad in [-0.5, 0.5]
layout(location = 1) in vec2 aUV;      // base UV, [0,1], v=0 at top

// Per-instance attributes (divisor 1).
layout(location = 2) in vec3 iPos;
layout(location = 3) in vec3 iVel;
layout(location = 4) in vec3 iAccel;
layout(location = 5) in float iMotionStart;
layout(location = 6) in vec2 iScale;
layout(location = 7) in vec2 iRot;     // x = yaw, y = pitch (radians)
layout(location = 8) in float iBillboard;
layout(location = 9) in vec4 iAnim;    // firstFrame, frameCount, fps, animStart
layout(location = 10) in vec4 iTint;   // RGBA multiplier (1,1,1,1 = opaque)
layout(location = 11) in float iSheet; // texture-array layer (sheet index)
layout(location = 12) in vec3 iPivot;  // xy = pivot offset (world units), z = xray flag

const int MAX_SHEETS = 16;             // keep in sync with Renderer::MAX_SHEETS

uniform mat4 uViewProj;
uniform float uTime;
// Per-sheet cell layout inside its layer: x = columns per row (after repack),
// yz = cell size in UV units, w unused. See texture.h / Renderer::loadSheet.
uniform vec4 uSheetGrid[MAX_SHEETS];
uniform vec3 uCamRight;
uniform vec3 uCamUp;
uniform vec3 uCamPos;
uniform vec3 uCamFwd;
uniform float uOrtho;      // 1 = orthographic camera (parallel eye rays)
uniform float uXrayPass;   // 1 = silhouette pass: only xray instances draw

// Grounded billboards are held this far above their anchor height so they
// never z-fight with a floor tile lying exactly at that height.
const float GROUND_EPS = 0.02;

out vec2 vUV;
out vec4 vTint;
flat out float vSheet;

void main() {
    // --- Motion: evolve the sprite center from its launch params -----------
    float dt = max(0.0, uTime - iMotionStart);
    vec3 center = iPos + iVel * dt + 0.5 * iAccel * dt * dt;

    // --- Orientation: billboard toward camera, or yaw+pitch basis ----------
    // Non-billboard quads use the model basis Ry(yaw) * Rx(pitch): pitch 0 is
    // upright (a wall), pitch -pi/2 lies the quad flat facing +Y (a floor).
    // The pivot offset shifts the quad within its own right/up plane, so one
    // anchor point (iPos) can carry a foot-anchored sprite or a row of glyphs.
    vec2 c = aCorner * iScale + iPivot.xy;
    vec3 offset;
    if (iBillboard > 0.5) {
        offset = uCamRight * c.x + uCamUp * c.y;
    } else {
        float cy = cos(iRot.x);
        float sy = sin(iRot.x);
        float cp = cos(iRot.y);
        float sp = sin(iRot.y);
        vec3 rightAxis = vec3(cy, 0.0, -sy);       // Ry(yaw) * +X
        vec3 upAxis = vec3(sy * sp, cp, cy * sp);  // Ry(yaw) * Rx(pitch) * +Y
        offset = rightAxis * c.x + upAxis * c.y;
    }
    vec3 world = center + offset;

    // Grounded billboard: a vertex that would sink below the anchor height is
    // slid toward the eye along its own view ray until it sits on that height.
    // Its screen position is unchanged; only its depth moves in front of the
    // floor, so a label hung below a character's feet stays visible.
    if (iBillboard > 1.5) {
        float floorY = center.y + GROUND_EPS;
        if (world.y < floorY) {
            if (uOrtho > 0.5) {
                if (uCamFwd.y < -0.001)
                    world -= uCamFwd * ((floorY - world.y) / -uCamFwd.y);
            } else if (uCamPos.y > floorY) {
                float t = (floorY - uCamPos.y) / (world.y - uCamPos.y);
                world = uCamPos + (world - uCamPos) * t;
            }
        }
    }
    gl_Position = uViewProj * vec4(world, 1.0);

    // Silhouette pass: push everything that is not flagged xray outside the
    // clip volume so it produces no fragments.
    if (uXrayPass > 0.5 && iPivot.z < 0.5)
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);

    // --- Animation: pick the current sheet cell from time ------------------
    float frameCount = max(iAnim.y, 1.0);
    float fps = iAnim.z;
    float animDt = max(0.0, uTime - iAnim.w);
    float local = (fps > 0.0) ? mod(floor(animDt * fps), frameCount) : 0.0;
    float cell = iAnim.x + local;

    vec4 grid = uSheetGrid[int(iSheet + 0.5)];
    float cols = grid.x;
    float col = mod(cell, cols);
    float row = floor(cell / cols);
    vUV  = (vec2(col, row) + aUV) * grid.yz;
    vTint = iTint;
    vSheet = iSheet;
}
)glsl";

static const char* FRAG_SRC = R"glsl(
in vec2 vUV;
in vec4 vTint;
flat in float vSheet;
out vec4 fragColor;

uniform highp sampler2DArray uTex;
uniform float uXrayPass;
uniform vec4 uXrayColor;

void main() {
    vec4 c = texture(uTex, vec3(vUV, vSheet)) * vTint;
    if (c.a < 0.05) discard;
    // Silhouette pass: flat colour, the sprite's own alpha as the mask.
    fragColor = (uXrayPass > 0.5) ? vec4(uXrayColor.rgb, uXrayColor.a * c.a) : c;
}
)glsl";

// Sky backdrop: one full-screen triangle (no vertex buffer — corners come from
// gl_VertexID), coloured per pixel from the direction of its view ray.
static const char* SKY_VERT_SRC = R"glsl(
out vec2 vNdc;

void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2)) * 2.0 - 1.0;
    vNdc = p;
    gl_Position = vec4(p, 1.0, 1.0);
}
)glsl";

static const char* SKY_FRAG_SRC = R"glsl(
in vec2 vNdc;
out vec4 fragColor;

uniform vec3 uCamRight;
uniform vec3 uCamUp;
uniform vec3 uCamFwd;
uniform float uTanHalfFov;
uniform float uAspect;
uniform float uOrtho;
uniform vec3 uZenith;
uniform vec3 uHorizon;
uniform vec3 uGround;

void main() {
    vec3 dir = uCamFwd;
    if (uOrtho < 0.5)
        dir = normalize(uCamFwd
                        + uCamRight * (vNdc.x * uAspect * uTanHalfFov)
                        + uCamUp * (vNdc.y * uTanHalfFov));
    // Above the horizon: ease from horizon haze up to the zenith colour.
    // Below it: fall off quickly into the ground colour. Arithmetic only (no
    // pow/exp), so desktop GL and WebGL agree to the last bit.
    float up = clamp(dir.y * 1.6, 0.0, 1.0);
    up = up * (2.0 - up);
    float down = clamp(-dir.y * 5.0, 0.0, 1.0);
    vec3 col = (dir.y >= 0.0) ? mix(uHorizon, uZenith, up)
                              : mix(uHorizon, uGround, down);
    fragColor = vec4(col, 1.0);
}
)glsl";

// Colour of the x-ray silhouette (see Instance::xray): a pale, mostly
// transparent wash so a hidden hero reads as "behind", not as a second sprite.
static const Vec4 XRAY_COLOR = {0.75f, 0.90f, 1.00f, 0.38f};

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------

bool Renderer::init() {
    program_ = buildProgram(VERT_SRC, FRAG_SRC);
    if (!program_) return false;

    skyProgram_ = buildProgram(SKY_VERT_SRC, SKY_FRAG_SRC);
    if (!skyProgram_) {
        glDeleteProgram(program_);
        program_ = 0;
        return false;
    }
    // Core profile needs a bound VAO even for an attribute-less draw.
    glGenVertexArrays(1, &skyVao_);

    textureArray_ = createTextureArray(SHEET_LAYER_SIZE, MAX_SHEETS);
    if (!textureArray_.valid()) {
        glDeleteProgram(program_);
        program_ = 0;
        return false;
    }

    // Unit quad: corner (x,y) + uv. v=0 at the top so sheets read top-down.
    const float quad[] = {
        // corner       uv
        -0.5f,  0.5f,   0.0f, 0.0f, // top-left
        -0.5f, -0.5f,   0.0f, 1.0f, // bottom-left
         0.5f, -0.5f,   1.0f, 1.0f, // bottom-right
         0.5f,  0.5f,   1.0f, 0.0f, // top-right
    };
    const unsigned int indices[] = {0, 1, 2, 0, 2, 3};

    glGenBuffers(1, &quadVbo_);
    glGenBuffers(1, &quadEbo_);
    glBindBuffer(GL_ARRAY_BUFFER, quadVbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);

    // One VAO + instance VBO per bucket (opaque / translucent), both sharing
    // the quad VBO/EBO. Two VAOs is the WebGL2-portable way to issue the two
    // passes — GLES 3.0 has no baseInstance to offset into a single buffer.
    for (Bucket& b : buckets_) {
        glGenVertexArrays(1, &b.vao);
        glGenBuffers(1, &b.vbo);
        glBindVertexArray(b.vao);

        glBindBuffer(GL_ARRAY_BUFFER, quadVbo_);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, quadEbo_);
        if (&b == &buckets_[0])  // EBO data uploads once; binding is per-VAO
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices,
                         GL_STATIC_DRAW);

        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                              (void*)(2 * sizeof(float)));
        glEnableVertexAttribArray(1);

        // Per-instance attributes — described directly off the Instance struct.
        glBindBuffer(GL_ARRAY_BUFFER, b.vbo);
        const GLsizei stride = sizeof(Instance);
        auto attrib = [&](GLuint loc, GLint size, size_t off) {
            glVertexAttribPointer(loc, size, GL_FLOAT, GL_FALSE, stride, (void*)off);
            glEnableVertexAttribArray(loc);
            glVertexAttribDivisor(loc, 1);
        };
        attrib(2, 3, offsetof(Instance, pos));
        attrib(3, 3, offsetof(Instance, vel));
        attrib(4, 3, offsetof(Instance, accel));
        attrib(5, 1, offsetof(Instance, motionStart));
        attrib(6, 2, offsetof(Instance, scale));
        attrib(7, 2, offsetof(Instance, rotation));  // vec2: (rotation=yaw, pitch)
        attrib(8, 1, offsetof(Instance, billboard));
        attrib(9, 4, offsetof(Instance, anim));
        attrib(10, 4, offsetof(Instance, tint));
        attrib(11, 1, offsetof(Instance, sheet));
        attrib(12, 3, offsetof(Instance, offset));    // vec3: (offset.xy, xray)

        glBindVertexArray(0);
    }

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // A sensible default camera until the world provides one.
    cameras_.push_back(Camera{});

    return true;
}

int Renderer::loadSheet(const char* path, int cols, int rows) {
    if (cols <= 0 || rows <= 0) {
        SDL_Log("Renderer::loadSheet: invalid grid cols=%d rows=%d for '%s'",
                cols, rows, path);
        return -1;
    }
    // Idempotent: the same sheet asked for twice (e.g. scene hot-reload) reuses
    // its layer instead of exhausting the array.
    for (size_t i = 0; i < sheets_.size(); ++i) {
        if (sheets_[i].path == path && sheets_[i].cols == cols &&
            sheets_[i].rows == rows) {
            return static_cast<int>(i);
        }
    }

    SheetGrid grid;
    int layer = loadSheetIntoArray(textureArray_, path, cols, rows, grid);
    if (layer < 0) return -1;

    LoadedSheet sheet;
    sheet.path = path;
    sheet.cols = cols;
    sheet.rows = rows;
    sheet.grid = grid;
    sheets_.push_back(std::move(sheet));
    return layer;
}

void Renderer::shutdown() {
    destroyTextureArray(textureArray_);
    for (Bucket& b : buckets_) {
        if (b.vbo) glDeleteBuffers(1, &b.vbo);
        if (b.vao) glDeleteVertexArrays(1, &b.vao);
    }
    if (quadEbo_) glDeleteBuffers(1, &quadEbo_);
    if (quadVbo_) glDeleteBuffers(1, &quadVbo_);
    if (skyVao_) glDeleteVertexArrays(1, &skyVao_);
    if (skyProgram_) glDeleteProgram(skyProgram_);
    if (program_) glDeleteProgram(program_);
    *this = Renderer{};
}

// ---------------------------------------------------------------------------
// State / diff
// ---------------------------------------------------------------------------

void Renderer::applyState(const WorldState& state) {
    idToSlot_.clear();
    for (Bucket& b : buckets_) {
        b.instances.clear();
        b.slotToId.clear();
        b.dirty = true;
    }
    for (const auto& [id, inst] : state.instances)
        insertInto(bucketFor(inst), id, inst);
    cameras_ = state.cameras;
    activeCamera_ = state.activeCamera;
    sky_ = state.sky;
}

void Renderer::applyDiff(const Diff& diff) {
    for (const auto& [id, inst] : diff.upserts) upsert(id, inst);
    for (EntityId id : diff.removals) remove(id);
    if (diff.replaceCameras && !diff.cameras.empty()) cameras_ = diff.cameras;
    if (diff.setActiveCamera) activeCamera_ = diff.activeCamera;
    // Bucket dirty flags are set by upsert/remove as needed.
}

void Renderer::insertInto(int bucket, EntityId id, const Instance& inst) {
    Bucket& b = buckets_[bucket];
    idToSlot_[id] = SlotRef{bucket, b.instances.size()};
    b.slotToId.push_back(id);
    b.instances.push_back(inst);
    b.dirty = true;
}

// Swap-remove within a bucket: move the last instance into the freed slot.
void Renderer::removeFromBucket(int bucket, size_t slot) {
    Bucket& b = buckets_[bucket];
    size_t last = b.instances.size() - 1;
    if (slot != last) {
        b.instances[slot] = b.instances[last];
        EntityId movedId = b.slotToId[last];
        b.slotToId[slot] = movedId;
        idToSlot_[movedId] = SlotRef{bucket, slot};
    }
    b.instances.pop_back();
    b.slotToId.pop_back();
    b.dirty = true;
}

void Renderer::upsert(EntityId id, const Instance& inst) {
    const int want = bucketFor(inst);
    auto it = idToSlot_.find(id);
    if (it == idToSlot_.end()) {
        insertInto(want, id, inst);
        return;
    }
    if (it->second.bucket == want) {
        Bucket& b = buckets_[want];
        b.instances[it->second.index] = inst;
        b.dirty = true;
        return;
    }
    // Opacity class changed (e.g. a live sprite becoming a translucent shim):
    // migrate the instance to the other bucket.
    removeFromBucket(it->second.bucket, it->second.index);
    insertInto(want, id, inst);
}

void Renderer::remove(EntityId id) {
    auto it = idToSlot_.find(id);
    if (it == idToSlot_.end()) return;
    SlotRef ref = it->second;
    idToSlot_.erase(it);
    removeFromBucket(ref.bucket, ref.index);
}

void Renderer::uploadBucket(Bucket& b) {
    glBindBuffer(GL_ARRAY_BUFFER, b.vbo);
    // Whole-buffer re-upload. Simple and plenty fast at our instance counts;
    // switch to ranged glBufferSubData only if profiling ever asks for it.
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(b.instances.size() * sizeof(Instance)),
                 b.instances.empty() ? nullptr : b.instances.data(),
                 GL_DYNAMIC_DRAW);
    b.dirty = false;

    // The silhouette pass re-draws the opaque bucket; skip it entirely unless
    // something in there actually asks for it.
    if (&b == &buckets_[OPAQUE]) {
        xrayCount_ = 0;
        for (const Instance& inst : b.instances)
            if (inst.xray > 0.5f) ++xrayCount_;
    }
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------

void Renderer::render(float time) {
    for (Bucket& b : buckets_)
        if (b.dirty) uploadBucket(b);

    glClearColor(0.08f, 0.08f, 0.12f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    Camera* cam = activeCamera();
    cam->aspect = viewportH_ > 0
                      ? static_cast<float>(viewportW_) / viewportH_
                      : 1.0f;
    const Vec3 camFwd = normalize(cam->target - cam->position);
    const float ortho = cam->projection == Projection::Orthographic ? 1.0f : 0.0f;

    if (sky_.enabled) renderSky(*cam, camFwd, ortho);

    if (buckets_[OPAQUE].instances.empty() &&
        buckets_[TRANSLUCENT].instances.empty())
        return;

    glUseProgram(program_);
    setUniform(program_, "uViewProj", cam->viewProjection());
    setUniform(program_, "uTime", time);
    setUniform(program_, "uCamRight", cam->right());
    setUniform(program_, "uCamUp", cam->trueUp());
    setUniform(program_, "uCamPos", cam->position);
    setUniform(program_, "uCamFwd", camFwd);
    setUniform(program_, "uOrtho", ortho);
    setUniform(program_, "uXrayPass", 0.0f);
    setUniform(program_, "uXrayColor", XRAY_COLOR);

    // Per-sheet cell layout for the UV math (x = cols, yz = cell UV size).
    Vec4 sheetGrid[MAX_SHEETS];
    for (int i = 0; i < MAX_SHEETS; ++i) {
        SheetGrid g;
        if (i < static_cast<int>(sheets_.size())) g = sheets_[i].grid;
        sheetGrid[i] = {g.cols, g.cellU, g.cellV, 0.0f};
    }
    setUniformArray(program_, "uSheetGrid", sheetGrid, MAX_SHEETS);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, textureArray_.id);
    setUniform(program_, "uTex", 0);

    // Pass 1 — opaque: depth write ON (blending stays enabled as always; the
    // alpha cutout in the fragment shader keeps sprite edges clean).
    if (!buckets_[OPAQUE].instances.empty()) {
        glDepthMask(GL_TRUE);
        glBindVertexArray(buckets_[OPAQUE].vao);
        glDrawElementsInstanced(
            GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr,
            static_cast<GLsizei>(buckets_[OPAQUE].instances.size()));
    }

    // Pass 1b — x-ray silhouettes: re-draw the flagged opaque instances where
    // they FAIL the depth test (something stands in front), as a flat wash.
    if (xrayCount_ > 0) {
        glDepthFunc(GL_GREATER);
        glDepthMask(GL_FALSE);
        setUniform(program_, "uXrayPass", 1.0f);
        glBindVertexArray(buckets_[OPAQUE].vao);
        glDrawElementsInstanced(
            GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr,
            static_cast<GLsizei>(buckets_[OPAQUE].instances.size()));
        setUniform(program_, "uXrayPass", 0.0f);
        glDepthFunc(GL_LESS);
        glDepthMask(GL_TRUE);
    }

    // Pass 2 — translucent (shims): depth test ON, depth write OFF, so ghosts
    // never punch holes in the depth buffer. No per-instance depth sorting
    // within this pass for now (shims sit at similar depths) — future work.
    if (!buckets_[TRANSLUCENT].instances.empty()) {
        glDepthMask(GL_FALSE);
        glBindVertexArray(buckets_[TRANSLUCENT].vao);
        glDrawElementsInstanced(
            GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr,
            static_cast<GLsizei>(buckets_[TRANSLUCENT].instances.size()));
        glDepthMask(GL_TRUE);  // restore: glClear needs the depth mask on
    }

    glBindVertexArray(0);
}

void Renderer::renderSky(const Camera& cam, Vec3 camFwd, float ortho) {
    glDisable(GL_DEPTH_TEST);  // backdrop: neither tests nor writes depth
    glUseProgram(skyProgram_);
    setUniform(skyProgram_, "uCamRight", cam.right());
    setUniform(skyProgram_, "uCamUp", cam.trueUp());
    setUniform(skyProgram_, "uCamFwd", camFwd);
    setUniform(skyProgram_, "uTanHalfFov", std::tan(cam.fovYRadians * 0.5f));
    setUniform(skyProgram_, "uAspect", cam.aspect);
    setUniform(skyProgram_, "uOrtho", ortho);
    setUniform(skyProgram_, "uZenith", sky_.zenith);
    setUniform(skyProgram_, "uHorizon", sky_.horizon);
    setUniform(skyProgram_, "uGround", sky_.ground);
    glBindVertexArray(skyVao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    glEnable(GL_DEPTH_TEST);
}

void Renderer::setViewport(int w, int h) {
    viewportW_ = w;
    viewportH_ = h;
    glViewport(0, 0, w, h);
}

Camera* Renderer::activeCamera() {
    if (cameras_.empty()) cameras_.push_back(Camera{});
    if (activeCamera_ < 0 || activeCamera_ >= static_cast<int>(cameras_.size())) {
        activeCamera_ = 0;
    }
    return &cameras_[activeCamera_];
}

} // namespace cv
