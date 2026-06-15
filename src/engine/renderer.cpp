#include "renderer.h"

#include <cstddef> // offsetof
#include <SDL.h>

#include "shader.h"

namespace cv {

// ---------------------------------------------------------------------------
// Shaders
// ---------------------------------------------------------------------------

static const char* VERT_SRC = R"glsl(
#version 330 core

// Shared quad geometry.
layout(location = 0) in vec2 aCorner;  // unit quad in [-0.5, 0.5]
layout(location = 1) in vec2 aUV;      // base UV, [0,1], v=0 at top

// Per-instance attributes (divisor 1).
layout(location = 2) in vec3 iPos;
layout(location = 3) in vec3 iVel;
layout(location = 4) in vec3 iAccel;
layout(location = 5) in float iMotionStart;
layout(location = 6) in vec2 iScale;
layout(location = 7) in float iRotation;
layout(location = 8) in float iBillboard;
layout(location = 9) in vec4 iAnim;    // firstFrame, frameCount, fps, animStart

uniform mat4 uViewProj;
uniform float uTime;
uniform int uSheetCols;
uniform int uSheetRows;
uniform vec3 uCamRight;
uniform vec3 uCamUp;

out vec2 vUV;

void main() {
    // --- Motion: evolve the sprite center from its launch params -----------
    float dt = max(0.0, uTime - iMotionStart);
    vec3 center = iPos + iVel * dt + 0.5 * iAccel * dt * dt;

    // --- Orientation: billboard toward camera, or yaw about world up -------
    vec2 c = aCorner * iScale;
    vec3 offset;
    if (iBillboard > 0.5) {
        offset = uCamRight * c.x + uCamUp * c.y;
    } else {
        float cs = cos(iRotation);
        float sn = sin(iRotation);
        vec3 rightAxis = vec3(cs, 0.0, -sn); // yaw in the XZ plane
        vec3 upAxis = vec3(0.0, 1.0, 0.0);
        offset = rightAxis * c.x + upAxis * c.y;
    }
    gl_Position = uViewProj * vec4(center + offset, 1.0);

    // --- Animation: pick the current sheet cell from time ------------------
    float frameCount = max(iAnim.y, 1.0);
    float fps = iAnim.z;
    float animDt = max(0.0, uTime - iAnim.w);
    float local = (fps > 0.0) ? mod(floor(animDt * fps), frameCount) : 0.0;
    float cell = iAnim.x + local;

    float cols = float(uSheetCols);
    float rows = float(uSheetRows);
    float col = mod(cell, cols);
    float row = floor(cell / cols);
    vUV = (vec2(col, row) + aUV) / vec2(cols, rows);
}
)glsl";

static const char* FRAG_SRC = R"glsl(
#version 330 core

in vec2 vUV;
out vec4 fragColor;

uniform sampler2D uTex;

void main() {
    vec4 c = texture(uTex, vUV);
    if (c.a < 0.05) discard; // clean sprite cutout
    fragColor = c;
}
)glsl";

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------

bool Renderer::init(const char* spriteSheetPath, int sheetCols, int sheetRows) {
    if (sheetCols <= 0 || sheetRows <= 0) {
        SDL_Log("Renderer::init: invalid sheet grid cols=%d rows=%d", sheetCols, sheetRows);
        return false;
    }
    sheetCols_ = sheetCols;
    sheetRows_ = sheetRows;

    program_ = buildProgram(VERT_SRC, FRAG_SRC);
    if (!program_) return false;

    texture_ = loadTexture(spriteSheetPath);
    if (!texture_.valid()) {
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

    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &quadVbo_);
    glGenBuffers(1, &quadEbo_);
    glGenBuffers(1, &instanceVbo_);

    glBindVertexArray(vao_);

    glBindBuffer(GL_ARRAY_BUFFER, quadVbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, quadEbo_);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);

    // Per-instance attributes — described directly off the Instance struct.
    glBindBuffer(GL_ARRAY_BUFFER, instanceVbo_);
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
    attrib(7, 1, offsetof(Instance, rotation));
    attrib(8, 1, offsetof(Instance, billboard));
    attrib(9, 4, offsetof(Instance, anim));

    glBindVertexArray(0);

    glEnable(GL_DEPTH_TEST);

    // A sensible default camera until the world provides one.
    cameras_.push_back(Camera{});

    return true;
}

void Renderer::shutdown() {
    destroyTexture(texture_);
    if (instanceVbo_) glDeleteBuffers(1, &instanceVbo_);
    if (quadEbo_) glDeleteBuffers(1, &quadEbo_);
    if (quadVbo_) glDeleteBuffers(1, &quadVbo_);
    if (vao_) glDeleteVertexArrays(1, &vao_);
    if (program_) glDeleteProgram(program_);
    *this = Renderer{};
}

// ---------------------------------------------------------------------------
// State / diff
// ---------------------------------------------------------------------------

void Renderer::applyState(const WorldState& state) {
    instances_.clear();
    idToSlot_.clear();
    slotToId_.clear();
    for (const auto& [id, inst] : state.instances) {
        idToSlot_[id] = instances_.size();
        slotToId_.push_back(id);
        instances_.push_back(inst);
    }
    cameras_ = state.cameras;
    activeCamera_ = state.activeCamera;
    instancesDirty_ = true;
}

void Renderer::applyDiff(const Diff& diff) {
    for (const auto& [id, inst] : diff.upserts) upsert(id, inst);
    for (EntityId id : diff.removals) remove(id);
    if (diff.replaceCameras && !diff.cameras.empty()) cameras_ = diff.cameras;
    if (diff.setActiveCamera) activeCamera_ = diff.activeCamera;
    // instancesDirty_ is set by upsert/remove as needed.
}

void Renderer::upsert(EntityId id, const Instance& inst) {
    auto it = idToSlot_.find(id);
    if (it == idToSlot_.end()) {
        idToSlot_[id] = instances_.size();
        slotToId_.push_back(id);
        instances_.push_back(inst);
    } else {
        instances_[it->second] = inst;
    }
    instancesDirty_ = true;
}

void Renderer::remove(EntityId id) {
    auto it = idToSlot_.find(id);
    if (it == idToSlot_.end()) return;

    // Swap-remove: move the last instance into the freed slot.
    size_t slot = it->second;
    size_t last = instances_.size() - 1;
    if (slot != last) {
        instances_[slot] = instances_[last];
        EntityId movedId = slotToId_[last];
        slotToId_[slot] = movedId;
        idToSlot_[movedId] = slot;
    }
    instances_.pop_back();
    slotToId_.pop_back();
    idToSlot_.erase(it);
    instancesDirty_ = true;
}

void Renderer::uploadInstances() {
    glBindBuffer(GL_ARRAY_BUFFER, instanceVbo_);
    // Whole-buffer re-upload. Simple and plenty fast at our instance counts;
    // switch to ranged glBufferSubData only if profiling ever asks for it.
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(instances_.size() * sizeof(Instance)),
                 instances_.empty() ? nullptr : instances_.data(),
                 GL_DYNAMIC_DRAW);
    instancesDirty_ = false;
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------

void Renderer::render(float time) {
    if (instancesDirty_) uploadInstances();

    glClearColor(0.08f, 0.08f, 0.12f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    if (instances_.empty()) return;

    Camera* cam = activeCamera();
    cam->aspect = viewportH_ > 0
                      ? static_cast<float>(viewportW_) / viewportH_
                      : 1.0f;

    glUseProgram(program_);
    setUniform(program_, "uViewProj", cam->viewProjection());
    setUniform(program_, "uTime", time);
    setUniform(program_, "uSheetCols", sheetCols_);
    setUniform(program_, "uSheetRows", sheetRows_);
    setUniform(program_, "uCamRight", cam->right());
    setUniform(program_, "uCamUp", cam->trueUp());

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture_.id);
    setUniform(program_, "uTex", 0);

    glBindVertexArray(vao_);
    glDrawElementsInstanced(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr,
                            static_cast<GLsizei>(instances_.size()));
    glBindVertexArray(0);
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
