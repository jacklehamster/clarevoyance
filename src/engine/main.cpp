// main.cpp — stress-test demo for the Clarevoyance graphics engine.
//
// Spawns a large grid of penguin billboard sprites (GRID_W × GRID_H) in a
// single applyState call. After that, nothing touches the instance buffer —
// animation runs entirely in the shader via uTime. FPS is shown in the title.
//
// Keys: C = toggle perspective/ortho  ESC = quit
//
// Test mode env vars (web: URL query params via web/pre.js):
//   CV_TEST_FRAMES=N   — render N frames then exit 0
//   CV_FIXED_TIME=T    — use constant t=T every frame (deterministic)
//   CV_SCREENSHOT=1    — dump framebuffer as base64 PNG on final test frame
#include <SDL.h>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#if !defined(__EMSCRIPTEN__)
#include <sys/stat.h>
#endif

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#include "renderer.h"
#include "screenshot.h"
#include "scene.h"
#include "events.h"
#include "input.h"

using namespace cv;

// Evaluate an instance's motion formula on the CPU (mirror of the GPU path):
//   pos(t) = pos + vel*(t-start) + 0.5*accel*(t-start)^2
static Vec3 positionAt(const Instance& inst, float t) {
    float dt = t - inst.motionStart;
    return inst.pos + inst.vel * dt + inst.accel * (0.5f * dt * dt);
}

// --- Sprite sheet -----------------------------------------------------------
// penguin.png: 4096×256, 16 cols × 1 row of 256×256 cells.
//   Walk(0-4)  Chat(4-5)  Surprised(6)  Confused(7)  Angry(8)
static const char* SHEET_PATH = "art/penguin.png";
static const int   SHEET_COLS = 16;
static const int   SHEET_ROWS = 1;
static const float ANIM_FPS   = 10.0f;

// --- Stress-test grid -------------------------------------------------------
// The grid always spans GRID_EXTENT × GRID_EXTENT world units so the camera
// view stays constant. Increasing GRID_W/H packs more penguins into the same
// area — density increases visibly as the count goes up.
static const int   GRID_W      = 30;
static const int   GRID_H      = 30;
static const float GRID_EXTENT = 20.0f;
static const float GRID_STEP   = GRID_EXTENT / GRID_W;

static const int WINDOW_W = 1280;
static const int WINDOW_H = 720;

// Five animation flavours to cycle through so the grid looks varied.
static const struct { int first; int count; } ANIMS[] = {
    {0, 5},  // Walk
    {4, 2},  // Chat
    {6, 1},  // Surprised
    {7, 1},  // Confused
    {8, 1},  // Angry
};
static const int NANIM = 5;

static std::vector<Camera> buildCameras(float angle) {
    // Camera always fits the full GRID_EXTENT × GRID_EXTENT area in view.
    const float R  = GRID_EXTENT * 0.75f;
    const float H  = GRID_EXTENT * 0.65f;
    Vec3 center = {GRID_EXTENT * 0.5f, 0.0f, GRID_EXTENT * 0.5f};
    Vec3 eye    = {
        center.x + std::sin(angle) * R,
        H,
        center.z + std::cos(angle) * R
    };

    Camera persp;
    persp.projection = Projection::Perspective;
    persp.position   = eye;
    persp.target     = center;

    Camera ortho;
    ortho.projection      = Projection::Orthographic;
    ortho.position        = eye;
    ortho.target          = center;
    ortho.orthoHalfHeight = GRID_EXTENT * 0.6f;

    return {persp, ortho};
}

// --- Loop context (shared between desktop while-loop and WASM callback) -----
struct LoopContext {
    SDL_Window* window;
    Renderer*   renderer;
    Uint64      startTicks;
    Uint64      fpsWindowStart;
    int         frameCount;
    int         activeCam;
    int         total;
    bool        running;
    // test mode
    int         testFrames;    // 0 = run forever
    float       fixedTime;     // <0 = use wall clock
    bool        doScreenshot;
    int         framesRendered;
    bool        testDone;      // one-shot guard: exit(0) queues more callbacks on WASM
    // scene mode (data-driven script layer); null in the stress-test demo
    Scene*       scene;
    EventSystem* events;
    std::unordered_map<EntityId, Instance>* entities;  // working copy for sim
    InputSource* inputSrc;   // pluggable input: keyboard, replay, null (cutscenes)
    // hot-reload (desktop scene mode only): re-load the scene file when its
    // mtime changes, checked once per second
    std::string scenePath;
    time_t      sceneMtime     = 0;
    Uint64      lastReloadCheck = 0;
};

#if !defined(__EMSCRIPTEN__)
// Once per second, stat() the scene file; if it changed on disk, re-load it and
// reset the world (renderer state, event system, input source). Desktop only —
// there is no file watching on the web (files live in the preloaded FS).
static void maybeHotReloadScene(LoopContext* ctx) {
    if (!ctx->scene || ctx->scenePath.empty()) return;

    Uint64 now = SDL_GetTicks64();
    if (now - ctx->lastReloadCheck < 1000) return;
    ctx->lastReloadCheck = now;

    struct stat st{};
    if (stat(ctx->scenePath.c_str(), &st) != 0) return;
    if (st.st_mtime == ctx->sceneMtime) return;
    ctx->sceneMtime = st.st_mtime;

    Scene fresh;
    std::string err;
    if (!loadScene(ctx->scenePath.c_str(), fresh, err)) {
        SDL_Log("Scene hot-reload failed (%s): %s — keeping current scene",
                ctx->scenePath.c_str(), err.c_str());
        return;
    }

    *ctx->scene = std::move(fresh);
    ctx->renderer->applyState(ctx->scene->initialState);
    ctx->events->init(*ctx->scene);
    // ctx->entities aliases the event system's working copy — same map object,
    // freshly refilled by init(), so the pointer stays valid.
    delete ctx->inputSrc;
    ctx->inputSrc = ctx->scene->bindings.empty()
        ? static_cast<InputSource*>(new NullInputSource())
        : new KeyboardInputSource(ctx->scene->bindings);
    SDL_Log("Scene hot-reloaded: %s", ctx->scenePath.c_str());
}
#endif

static void frame(void* arg) {
    LoopContext* ctx = static_cast<LoopContext*>(arg);

    // Collect all SDL events this frame. Engine-level events (quit, camera
    // toggle) are handled here; the rest are forwarded to the input source.
    std::vector<SDL_Event> sdlEvents;
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) ctx->running = false;
        if (event.type == SDL_KEYDOWN) {
            if (event.key.keysym.sym == SDLK_ESCAPE) ctx->running = false;
            if (event.key.keysym.sym == SDLK_c && event.key.repeat == 0)
                ctx->activeCam = (ctx->activeCam + 1) % 2;
        }
        if (event.type == SDL_WINDOWEVENT &&
            (event.window.event == SDL_WINDOWEVENT_RESIZED ||
             event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED)) {
            int w = event.window.data1;
            int h = event.window.data2;
            ctx->renderer->setViewport(w, h);
        }
        sdlEvents.push_back(event);
    }

    float t = ctx->fixedTime >= 0.0f
        ? ctx->fixedTime
        : (float)(SDL_GetTicks64() - ctx->startTicks) / 1000.0f;

#if !defined(__EMSCRIPTEN__)
    maybeHotReloadScene(ctx);
#endif

    if (ctx->scene) {
        // Data-driven script layer: advance the sim, apply keyboard movement,
        // evaluate events, apply the resulting Diff. The camera is fixed by the
        // scene (no orbit). ctx->entities aliases the event system's working copy,
        // so movement, events and the renderer all see one logical state.
        std::unordered_map<EntityId, Vec3> positions;
        for (const auto& kv : *ctx->entities)
            positions[kv.first] = positionAt(kv.second, t);

        ResolvedActions actions = ctx->inputSrc->poll(sdlEvents);

        Diff diff;
        // Free movement: applies a velocity to every controlled entity.
        applyMovement(actions, ctx->events->attrs(), *ctx->entities, t, positions, diff);
        // Events (including input triggers) run against the resolved actions.
        ctx->events->update(*ctx->scene, t, positions, actions, diff);
        // Keep removals in sync (movement/event upserts already write *ctx->entities
        // since it aliases the event system's working copy).
        for (const auto& up : diff.upserts) (*ctx->entities)[up.first] = up.second;
        for (EntityId id : diff.removals)   ctx->entities->erase(id);
        if (!diff.upserts.empty() || !diff.removals.empty())
            ctx->renderer->applyDiff(diff);
    } else {
        // Stress-test demo: the only thing that changes each frame is the
        // camera orbit (uniforms only).
        Diff diff;
        diff.replaceCameras  = true;
        diff.cameras         = buildCameras(t * 0.1f);
        diff.setActiveCamera = true;
        diff.activeCamera    = ctx->activeCam;
        ctx->renderer->applyDiff(diff);
    }

    ctx->renderer->render(t);

    // Test-mode checks run after render() but before SwapWindow (back buffer valid).
    ctx->framesRendered++;
    bool lastTestFrame = ctx->testFrames > 0 && ctx->framesRendered >= ctx->testFrames
                         && !ctx->testDone;
    if (lastTestFrame) {
        ctx->testDone = true;
        GLenum err = glGetError();
        if (err != GL_NO_ERROR)
            SDL_Log("CV_GLERROR: 0x%x", err);

        if (!framebufferNonBlank(WINDOW_W, WINDOW_H))
            SDL_Log("CV_BLANK: framebuffer appears empty");

        if (ctx->doScreenshot)
            captureFramebufferBase64(WINDOW_W, WINDOW_H);

        ctx->running = false;
#ifdef __EMSCRIPTEN__
        exit(0);  // signals emrun EXIT_STATUS:0 and flushes stdout
#endif
    }

    SDL_GL_SwapWindow(ctx->window);

    // Update title with FPS once per second.
    ctx->frameCount++;
    Uint64 now = SDL_GetTicks64();
    float elapsed = (float)(now - ctx->fpsWindowStart) / 1000.0f;
    if (elapsed >= 1.0f) {
        float fps = ctx->frameCount / elapsed;
        std::string title =
            "Clarevoyance — " + std::to_string(ctx->total) + " penguins  |  " +
            std::to_string((int)std::round(fps)) + " FPS" +
            "  (C: toggle camera  ESC: quit)";
        SDL_SetWindowTitle(ctx->window, title.c_str());
        SDL_Log("FPS: %.1f  (%d penguins)", fps, ctx->total);
        ctx->frameCount    = 0;
        ctx->fpsWindowStart = now;
    }

}

int main(int, char**) {
    // --- Parse test-mode env vars -------------------------------------------
    int   testFrames  = 0;
    float fixedTime   = -1.0f;
    bool  doScreenshot = false;
    std::string scenePath;   // CV_SCENE=path → run the data-driven scene instead of the demo

#ifdef __EMSCRIPTEN__
    // Read test params from URL query string directly — ENV scoping in
    // Emscripten 6.0 closures makes getenv() unreliable from pre.js.
    {
        char buf[256] = {};
        EM_ASM({
            var p = new URLSearchParams(location.search);
            var v;
            if ((v = p.get('CV_TEST_FRAMES'))) stringToUTF8(v, $0, 64);
            if ((v = p.get('CV_FIXED_TIME')))  stringToUTF8(v, $1, 64);
            if ((v = p.get('CV_SCREENSHOT')))  stringToUTF8(v, $2, 64);
            if ((v = p.get('CV_SCENE')))       stringToUTF8(v, $3, 128);
        }, buf, buf + 16, buf + 32, buf + 48);
        if (buf[0])    testFrames   = std::atoi(buf);
        if (buf[16])   fixedTime    = std::atof(buf + 16);
        if (buf[32])   doScreenshot = (std::atoi(buf + 32) != 0);
        if (buf[48])   scenePath    = (buf + 48);
        SDL_Log("Test mode: frames=%d fixedTime=%.1f screenshot=%d",
                testFrames, fixedTime, doScreenshot ? 1 : 0);
    }
#else
    if (const char* v = getenv("CV_TEST_FRAMES"))  testFrames   = std::atoi(v);
    if (const char* v = getenv("CV_FIXED_TIME"))   fixedTime    = std::atof(v);
    if (const char* v = getenv("CV_SCREENSHOT"))   doScreenshot = (std::atoi(v) != 0);
    if (const char* v = getenv("CV_SCENE"))        scenePath    = v;
#endif

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
#ifdef __EMSCRIPTEN__
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
#else
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
#endif

    SDL_Window* window = SDL_CreateWindow(
        "Clarevoyance — stress test",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        WINDOW_W, WINDOW_H, SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN);
    if (!window) { SDL_Quit(); return 1; }

    SDL_GLContext glCtx = SDL_GL_CreateContext(window);
    if (!glCtx) { SDL_DestroyWindow(window); SDL_Quit(); return 1; }
    SDL_GL_SetSwapInterval(1);

#if !defined(__EMSCRIPTEN__) && !defined(__APPLE__)
    glewExperimental = GL_TRUE;
    GLenum glewErr = glewInit();
    if (glewErr != GLEW_OK) {
        SDL_Log("GLEW init failed: %s", glewGetErrorString(glewErr));
        SDL_GL_DeleteContext(glCtx); SDL_DestroyWindow(window); SDL_Quit();
        return 1;
    }
#endif

    SDL_Log("OpenGL: %s", glGetString(GL_VERSION));

    // --- Optionally load a data-driven scene (the script layer) -------------
    // Heap-allocated so they outlive main() on the WASM async loop.
    Scene*       scene   = nullptr;
    EventSystem* events  = nullptr;
    std::unordered_map<EntityId, Instance>* sceneEntities = nullptr;
    InputSource* inputSrc = nullptr;  // set after scene is loaded

    const char* sheetPath = SHEET_PATH;
    int sheetCols = SHEET_COLS, sheetRows = SHEET_ROWS;

    if (!scenePath.empty()) {
        scene = new Scene();
        std::string err;
        if (!loadScene(scenePath.c_str(), *scene, err)) {
            SDL_Log("Scene load failed (%s): %s", scenePath.c_str(), err.c_str());
            delete scene;
            SDL_GL_DeleteContext(glCtx); SDL_DestroyWindow(window); SDL_Quit();
            return 1;
        }
        sheetPath = scene->sheet.path.c_str();
        sheetCols = scene->sheet.cols;
        sheetRows = scene->sheet.rows;
        SDL_Log("Loaded scene '%s': %zu entities, %zu events, %zu bindings",
                scenePath.c_str(), scene->initialState.instances.size(),
                scene->events.size(), scene->bindings.size());
    }

    Renderer renderer;
    if (!renderer.init(sheetPath, sheetCols, sheetRows)) {
        SDL_Log("Renderer init failed — is %s present?", sheetPath);
        delete scene;
        SDL_GL_DeleteContext(glCtx); SDL_DestroyWindow(window); SDL_Quit();
        return 1;
    }
    renderer.setViewport(WINDOW_W, WINDOW_H);

    int total = 0;
    if (scene) {
        // --- Scene mode: upload the scene's world, prime the event system ---
        renderer.applyState(scene->initialState);
        events = new EventSystem();
        events->init(*scene);
        // Alias the event system's working copy so movement, events and the
        // renderer all share one logical instance map (no second copy to sync).
        sceneEntities = &events->entities();
        total = static_cast<int>(scene->initialState.instances.size());
        // Choose input source: keyboard when bindings are defined, null otherwise.
        if (!scene->bindings.empty())
            inputSrc = new KeyboardInputSource(scene->bindings);
        else
            inputSrc = new NullInputSource();
    } else {
        // Stress-test demo has no scene; use a NullInputSource as placeholder.
        inputSrc = new NullInputSource();
        // --- Stress-test demo: a dense grid of penguins, uploaded once ------
        WorldState state;
        state.cameras     = buildCameras(0.0f);
        state.activeCamera = 0;

        total = GRID_W * GRID_H;
        SDL_Log("Spawning %d penguins…", total);

        EntityId id = 1;
        for (int row = 0; row < GRID_H; ++row) {
            for (int col = 0; col < GRID_W; ++col, ++id) {
                float x = col * GRID_STEP;
                float z = row * GRID_STEP;
                float animOffset = -(col + row * GRID_W) * (1.0f / ANIM_FPS);
                int aIdx = (col + row) % NANIM;

                float s = GRID_STEP * 0.9f;
                Instance inst = makeBillboard({x, s * 0.5f, z}, {s, s});
                setAnimation(inst, ANIMS[aIdx].first, ANIMS[aIdx].count,
                             ANIM_FPS, animOffset);
                state.instances[id] = inst;
            }
        }
        renderer.applyState(state);  // one upload — never touched again
    }
    SDL_Log("Buffer uploaded. Rendering %d instances per frame.", total);

    // --- Run loop -----------------------------------------------------------
    Uint64 startTicks = SDL_GetTicks64();

#ifdef __EMSCRIPTEN__
    // Heap-allocate so the context outlives main() on the web.
    LoopContext* ctx = new LoopContext{
        window, &renderer,
        startTicks, startTicks,
        0, 0, total, true,
        testFrames, fixedTime, doScreenshot, 0, false,
        scene, events, sceneEntities, inputSrc,
        {}, 0, 0
    };
    // fps=0  → requestAnimationFrame (display-synced, pauses in background) — interactive
    // fps=60 → setTimeout at 60 fps (runs even in hidden tabs)           — test mode
    // simulate_infinite_loop=1: main() never returns; exit(0) terminates and
    // signals emrun with EXIT_STATUS:0. Required with EXIT_RUNTIME=1 + --emrun.
    int loopFps = (ctx->testFrames > 0) ? 60 : 0;
    emscripten_set_main_loop_arg(frame, ctx, loopFps, 1);
#else
    LoopContext ctx{
        window, &renderer,
        startTicks, startTicks,
        0, 0, total, true,
        testFrames, fixedTime, doScreenshot, 0, false,
        scene, events, sceneEntities, inputSrc,
        {}, 0, 0
    };
    if (scene) {
        ctx.scenePath = scenePath;
        struct stat st{};
        if (stat(scenePath.c_str(), &st) == 0) ctx.sceneMtime = st.st_mtime;
    }
    while (ctx.running) frame(&ctx);

    renderer.shutdown();
    SDL_GL_DeleteContext(glCtx);
    SDL_DestroyWindow(window);
    SDL_Quit();
    // sceneEntities aliases events->entities() — owned by `events`, not freed here.
    delete ctx.inputSrc;  // via ctx: hot-reload may have replaced the input source
    delete events;
    delete scene;
#endif
    return 0;
}
