// main.cpp — the Clarevoyance executable: window, GL context, main loop.
//
// Two things can run in it:
//
//   - a data-driven SCENE (CV_SCENE=path): the game layer loads the JSON, the
//     fixed-tick sim advances it, and this file feeds it input and forwards the
//     resulting Diffs to the renderer. Scenes with a "cameraRig" get the follow
//     camera (top-down / dungeon views); scenes with "terrain" get their ground
//     streamed in around the hero.
//   - the STRESS TEST (no CV_SCENE): a dense grid of animated penguins under an
//     orbiting camera, uploaded once and never touched again.
//
// Config comes from env vars on desktop and URL query params on the web:
//   CV_SCENE=path      — scene file to run
//   CV_VIEW=topdown|dungeon — override the scene's starting camera-rig view
//   CV_TEST_FRAMES=N   — render N frames then exit 0
//   CV_FIXED_TIME=T    — use constant t=T every frame (deterministic)
//   CV_SCREENSHOT=1    — dump framebuffer as base64 PNG on final test frame
//
// Keys handled here: ESC = quit, C = stress-test camera toggle. Everything
// else goes to the scene's own key bindings.
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

#include "camera_rig.h"
#include "renderer.h"
#include "screenshot.h"
#include "sdl_input.h"
#include "scene.h"
#include "events.h"
#include "input.h"
#include "sim.h"
#include "terrain.h"

using namespace cv;

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

// Translucent "ghost" penguins hovering over the grid — exercises the
// translucent render pass (depth test on, depth write off; see renderer.cpp).
static const int   NGHOSTS          = 3;
static const float GHOST_TINT_ALPHA = 0.4f;
static const float GHOST_SCALE      = GRID_EXTENT * 0.15f;

static const int WINDOW_W = 1280;
static const int WINDOW_H = 720;

// Accumulator cap: after a pause/breakpoint/background tab, never try to catch
// up more than this much sim time in one frame (avoids the spiral of death).
static const double MAX_ACCUM = 0.25;

// Follow camera: when a wall stops hiding the hero the camera drifts back out
// to its full distance at this rate (per second, exponential). Moving IN is
// instant — the hero must never be hidden, even for a frame.
static const float CAM_RELAX_RATE = 5.0f;

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
    // scene mode (data-driven script layer); null in the stress-test demo.
    // The Scene is immutable after load (hot-reload replaces the whole object);
    // ALL runtime mutation lives in the copyable SimState.
    Scene*       scene;
    SimState*    sim;
    InputSource* inputSrc;   // pluggable input: bindings, replay, null (cutscenes)
    InputFrame  prevInput = {};  // last frame's key state (edge detection across frames)
    // fixed-timestep sim: wall time fills an accumulator; the sim advances in
    // whole SIM_DT ticks; rendering uses the continuous time clock.time()+accum
    SimClock    clock = {};
    double      accum = 0.0;
    Uint64      lastFrameTicks = 0;  // wall-clock ms at previous frame (SDL_GetTicks64)
    // Pending tick-stamped input command: down-state mirrors the latest frame;
    // pressed/released edges accumulate until a sim tick consumes them (so an
    // edge landing on a frame that runs zero ticks is not lost, and edges never
    // repeat across multiple ticks within one frame).
    InputFrame  pending = {};
    // hot-reload (desktop scene mode only): re-load the scene file when its
    // mtime changes, checked once per second
    std::string scenePath = {};
    time_t      sceneMtime     = 0;
    Uint64      lastReloadCheck = 0;
    // world view (scenes with terrain and/or a camera rig) — render-side only,
    // none of it feeds back into the sim
    TerrainStreamer streamer = {};
    Vec3        focus = {0, 0, 0};   // what the camera/terrain follow; kept if the entity vanishes
    float       camPull = 1.0f;      // smoothed camera distance fraction (see rigCamera)
    bool        camPullValid = false;
    float       camDrop = 0.0f;      // smoothed downward tilt while a dialogue is open
    int         viewOverride = -1;   // CV_VIEW: RIG_TOPDOWN / RIG_DUNGEON, or -1
    std::string sceneName = {};      // file name shown in the window title
};

// Scenes with a world (terrain and/or camera rig): keep the ground streamed in
// around the followed entity and point the follow camera at it. Runs once per
// rendered frame at the continuous render time, so the camera glides with the
// analytic motion instead of stepping with the sim ticks.
static void updateWorldView(LoopContext* ctx, float t, float frameDt) {
    const Scene& scene = *ctx->scene;
    if (!scene.rig.enabled && !scene.terrain.enabled) return;

    auto followed = ctx->sim->entities.find(scene.rigFollow);
    if (followed != ctx->sim->entities.end())
        ctx->focus = motionAt(followed->second, t);
    else if (scene.rigFollow == 0 && !scene.initialState.cameras.empty())
        ctx->focus = scene.initialState.cameras[0].target;

    Diff diff;
    ctx->streamer.update(scene.terrain, ctx->focus, diff);

    if (scene.rig.enabled) {
        const RigPose pose = rigPose(scene.rig, ctx->sim->rig, t, ctx->focus);
        // In the dungeon view a wall between the eye and the hero pulls the
        // camera in front of it. The high top-down view looks over the walls
        // (the hero's x-ray silhouette covers that case), so the pull fades
        // out with the view blend.
        const float clear = scene.terrain.sightClearance(pose.pivot, pose.eye);
        const float want  = 1.0f - (1.0f - clear) * rigBlend(ctx->sim->rig, t);
        if (!ctx->camPullValid || want < ctx->camPull) {
            ctx->camPull = want;
            ctx->camPullValid = true;
        } else {
            ctx->camPull += (want - ctx->camPull)
                          * (1.0f - std::exp(-CAM_RELAX_RATE * frameDt));
        }
        // A dialogue window covers the bottom of the screen: in the dungeon
        // view, tilt down a little so the hero stands above it.
        const float wantDrop = ctx->sim->talk.active
            ? RIG_TALK_DROP * rigBlend(ctx->sim->rig, t) : 0.0f;
        ctx->camDrop += (wantDrop - ctx->camDrop)
                      * (1.0f - std::exp(-CAM_RELAX_RATE * frameDt));
        diff.replaceCameras  = true;
        diff.cameras         = {rigCamera(pose, ctx->camPull, ctx->camDrop)};
        diff.setActiveCamera = true;
        diff.activeCamera    = 0;
    }
    ctx->renderer->applyDiff(diff);
}

// Everything that has to happen when a scene becomes current: apply CV_VIEW,
// upload its initial world, build fresh sim state and input source.
static void startScene(LoopContext* ctx) {
    Scene& scene = *ctx->scene;
    if (ctx->viewOverride >= 0) scene.rig.startView = ctx->viewOverride;

    Diff dropTerrain;
    ctx->streamer.reset(dropTerrain);       // ids die with applyState below
    ctx->renderer->applyState(scene.initialState);

    if (ctx->sim) *ctx->sim = makeSimState(scene);
    else          ctx->sim = new SimState(makeSimState(scene));

    delete ctx->inputSrc;
    // The source copies the bindings, so it stays valid across scene reloads.
    ctx->inputSrc = scene.bindings.empty()
        ? static_cast<InputSource*>(new NullInputSource())
        : new BindingsInputSource(scene.bindings);
    ctx->prevInput = InputFrame{};
    ctx->pending   = InputFrame{};
    ctx->camPullValid = false;
    ctx->camDrop = 0.0f;
    ctx->total = static_cast<int>(scene.initialState.instances.size());
}

// Load every sheet a scene references into the renderer's texture array; the
// entity `sheet` field indexes the scene's sheet list. loadSheet is idempotent,
// so calling this again on hot-reload is cheap. Warns if a sheet lands on a
// layer other than its scene index (only possible when a hot-reload *changes*
// the sheet list — entity sheet indices assume the identity mapping).
static bool loadSceneSheets(Renderer& renderer, const Scene& scene) {
    for (size_t i = 0; i < scene.sheets.size(); ++i) {
        const SheetInfo& s = scene.sheets[i];
        int layer = renderer.loadSheet(s.path.c_str(), s.cols, s.rows);
        if (layer < 0) return false;
        if (layer != static_cast<int>(i))
            SDL_Log("Warning: sheet '%s' is layer %d but scene index %zu — "
                    "restart to realign sheet indices", s.path.c_str(), layer, i);
    }
    return true;
}

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
    if (!loadSceneSheets(*ctx->renderer, *ctx->scene))
        SDL_Log("Scene hot-reload: sheet load failed — sprites may be blank");
    startScene(ctx);                        // fresh runtime state, tick 0
    SDL_Log("Scene hot-reloaded: %s", ctx->scenePath.c_str());
}
#endif

// One simulation tick for the scene mode: consume the pending tick command,
// step the sim (movement + events — see stepSim in src/game/sim.cpp), and
// forward the resulting Diff to the renderer. Pure CPU — sim time comes from
// the SimState's tick clock, never the wall clock.
static void tickScene(LoopContext* ctx) {
    // Resolve the tick command — a pure function of the pending frame and the
    // bindings. Edges are cleared after this tick so pressed/released never
    // repeat across ticks; down-state persists.
    ResolvedActions actions = ctx->inputSrc->poll(ctx->pending);
    ctx->pending.pressed.clear();
    ctx->pending.released.clear();

    Diff diff;
    stepSim(*ctx->scene, *ctx->sim, actions, diff);
    if (!diff.upserts.empty() || !diff.removals.empty())
        ctx->renderer->applyDiff(diff);
}

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

#if !defined(__EMSCRIPTEN__)
    maybeHotReloadScene(ctx);
#endif

    // The ENGINE translates SDL events into the device-agnostic InputFrame once
    // per rendered frame; the game-layer input source only ever sees key names,
    // never SDL. Edges merge into the pending tick command until consumed.
    InputFrame inputFrame = buildInputFrame(sdlEvents, ctx->prevInput);
    ctx->prevInput = inputFrame;
    ctx->pending.down = inputFrame.down;
    ctx->pending.pressed.insert(inputFrame.pressed.begin(), inputFrame.pressed.end());
    ctx->pending.released.insert(inputFrame.released.begin(), inputFrame.released.end());

    // Advance the accumulator from the wall clock (capped), then step the sim
    // in whole SIM_DT ticks. CV_FIXED_TIME maps to a fixed tick count instead:
    // the clock was pinned at startup and one step runs per frame at that
    // constant sim time, so test screenshots stay deterministic.
    Uint64 nowMs = SDL_GetTicks64();
    float frameDt = 0.0f;   // wall time since the previous frame (0 in test mode)
    if (ctx->fixedTime < 0.0f) {
        frameDt = (float)(nowMs - ctx->lastFrameTicks) / 1000.0f;
        ctx->accum += frameDt;
        if (ctx->accum > MAX_ACCUM) ctx->accum = MAX_ACCUM;
    }
    ctx->lastFrameTicks = nowMs;

    if (ctx->fixedTime >= 0.0f) {
        // Deterministic test mode: the clock stays pinned at the fixed tick;
        // one step runs per rendered frame at that constant sim time.
        if (ctx->scene) {
            uint64_t pinned = ctx->sim->clock.tick;
            tickScene(ctx);
            ctx->sim->clock.tick = pinned;
        }
    } else {
        while (ctx->accum >= SIM_DT) {
            if (ctx->scene) {
                tickScene(ctx);           // advances ctx->sim->clock
            } else {
                ctx->clock.tick++;        // stress demo: time flows, no sim work
            }
            ctx->accum -= SIM_DT;
        }
    }

    // Render at the CONTINUOUS time — sim time plus the unsimulated remainder —
    // so visuals never stutter. The analytic motion model (pos + vel*t + ½at²)
    // makes this exact, not an approximation.
    double simNow = ctx->sim ? ctx->sim->clock.time() : ctx->clock.time();
    float t = ctx->fixedTime >= 0.0f
        ? ctx->fixedTime
        : static_cast<float>(simNow + ctx->accum);

    if (ctx->scene) {
        updateWorldView(ctx, t, frameDt);
    } else {
        // Stress-test demo: the only thing that changes each frame is the
        // camera orbit (uniforms only) — render-side, driven by continuous time.
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
        std::string what = ctx->scene
            ? ctx->sceneName
            : std::to_string(ctx->total) + " penguins  (C: toggle camera)";
        std::string title = "Clarevoyance — " + what + "  |  " +
            std::to_string((int)std::round(fps)) + " FPS  (ESC: quit)";
        SDL_SetWindowTitle(ctx->window, title.c_str());
        SDL_Log("FPS: %.1f  (%s)", fps, what.c_str());
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
    std::string viewName;    // CV_VIEW=topdown|dungeon → starting camera-rig view

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
            if ((v = p.get('CV_VIEW')))        stringToUTF8(v, $4, 16);
        }, buf, buf + 16, buf + 32, buf + 48, buf + 192);
        if (buf[0])    testFrames   = std::atoi(buf);
        if (buf[16])   fixedTime    = std::atof(buf + 16);
        if (buf[32])   doScreenshot = (std::atoi(buf + 32) != 0);
        if (buf[48])   scenePath    = (buf + 48);
        if (buf[192])  viewName     = (buf + 192);
        SDL_Log("Test mode: frames=%d fixedTime=%.1f screenshot=%d",
                testFrames, fixedTime, doScreenshot ? 1 : 0);
    }
#else
    if (const char* v = getenv("CV_TEST_FRAMES"))  testFrames   = std::atoi(v);
    if (const char* v = getenv("CV_FIXED_TIME"))   fixedTime    = std::atof(v);
    if (const char* v = getenv("CV_SCREENSHOT"))   doScreenshot = (std::atoi(v) != 0);
    if (const char* v = getenv("CV_SCENE"))        scenePath    = v;
    if (const char* v = getenv("CV_VIEW"))         viewName     = v;
#endif

    int viewOverride = -1;
    if (viewName == "topdown")      viewOverride = RIG_TOPDOWN;
    else if (viewName == "dungeon") viewOverride = RIG_DUNGEON;
    else if (!viewName.empty())
        SDL_Log("Ignoring CV_VIEW='%s' (expected topdown or dungeon)", viewName.c_str());

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
        "Clarevoyance",
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
    // Heap-allocated so they outlive main() on the WASM async loop. The Scene
    // is immutable after load; all runtime mutation lives in the SimState.
    Scene*       scene   = nullptr;
    SimState*    sim     = nullptr;
    InputSource* inputSrc = nullptr;  // set after scene is loaded

    if (!scenePath.empty()) {
        scene = new Scene();
        std::string err;
        if (!loadScene(scenePath.c_str(), *scene, err)) {
            SDL_Log("Scene load failed (%s): %s", scenePath.c_str(), err.c_str());
            delete scene;
            SDL_GL_DeleteContext(glCtx); SDL_DestroyWindow(window); SDL_Quit();
            return 1;
        }
        SDL_Log("Loaded scene '%s': %zu entities, %zu events, %zu bindings",
                scenePath.c_str(), scene->initialState.instances.size(),
                scene->events.size(), scene->bindings.size());
    }

    Renderer renderer;
    if (!renderer.init()) {
        SDL_Log("Renderer init failed");
        delete scene;
        SDL_GL_DeleteContext(glCtx); SDL_DestroyWindow(window); SDL_Quit();
        return 1;
    }
    renderer.setViewport(WINDOW_W, WINDOW_H);

    // Load sprite sheets after init — sheets are runtime resources now, one
    // texture-array layer each (scene sheets, or the stress-test penguin sheet).
    bool sheetsOk = scene
        ? loadSceneSheets(renderer, *scene)
        : renderer.loadSheet(SHEET_PATH, SHEET_COLS, SHEET_ROWS) >= 0;
    if (!sheetsOk) {
        SDL_Log("Sprite sheet load failed");
        renderer.shutdown();
        delete scene;
        SDL_GL_DeleteContext(glCtx); SDL_DestroyWindow(window); SDL_Quit();
        return 1;
    }

    // --- Loop context: heap-allocated so it outlives main() on the web ------
    Uint64 startTicks = SDL_GetTicks64();
    LoopContext* ctx = new LoopContext{
        window, &renderer,
        startTicks, startTicks,
        0, 0, 0, true,
        testFrames, fixedTime, doScreenshot, 0, false,
        scene, sim, inputSrc
    };
    ctx->lastFrameTicks = startTicks;
    ctx->viewOverride = viewOverride;

    if (scene) {
        // --- Scene mode: upload the scene's world, prime the sim state ------
        size_t slash = scenePath.find_last_of('/');
        ctx->sceneName = slash == std::string::npos ? scenePath : scenePath.substr(slash + 1);
        startScene(ctx);
    } else {
        // Stress-test demo has no scene; use a NullInputSource as placeholder.
        ctx->inputSrc = new NullInputSource();
        // --- Stress-test demo: a dense grid of penguins, uploaded once ------
        WorldState state;
        state.cameras     = buildCameras(0.0f);
        state.activeCamera = 0;

        int total = GRID_W * GRID_H;
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

        // A few translucent ghosts floating over the crowd (shim-style tint).
        for (int g = 0; g < NGHOSTS; ++g, ++id) {
            float x = GRID_EXTENT * (0.3f + 0.2f * g);
            float z = GRID_EXTENT * 0.5f;
            Instance ghost = makeBillboard(
                {x, GHOST_SCALE * 0.5f + 0.5f, z}, {GHOST_SCALE, GHOST_SCALE});
            ghost.tint = {1.0f, 1.0f, 1.0f, GHOST_TINT_ALPHA};
            setAnimation(ghost, ANIMS[0].first, ANIMS[0].count, ANIM_FPS,
                         -0.13f * g);
            state.instances[id] = ghost;
        }
        total += NGHOSTS;
        ctx->total = total;

        renderer.applyState(state);  // one upload — never touched again
    }
    SDL_Log("Buffer uploaded. Rendering %d instances per frame.", ctx->total);

    // --- Run loop -----------------------------------------------------------
    // CV_FIXED_TIME maps to a fixed tick count: tick = round(T / SIM_DT).
    if (fixedTime >= 0.0f) {
        uint64_t fixedTick = static_cast<uint64_t>(std::llround(fixedTime / SIM_DT));
        ctx->clock.tick = fixedTick;
        if (ctx->sim) ctx->sim->clock.tick = fixedTick;
    }

#ifdef __EMSCRIPTEN__
    // fps=0  → requestAnimationFrame (display-synced, pauses in background) — interactive
    // fps=60 → setTimeout at 60 fps (runs even in hidden tabs)           — test mode
    // simulate_infinite_loop=1: main() never returns; exit(0) terminates and
    // signals emrun with EXIT_STATUS:0. Required with EXIT_RUNTIME=1 + --emrun.
    int loopFps = (ctx->testFrames > 0) ? 60 : 0;
    emscripten_set_main_loop_arg(frame, ctx, loopFps, 1);
#else
    if (scene) {
        ctx->scenePath = scenePath;
        struct stat st{};
        if (stat(scenePath.c_str(), &st) == 0) ctx->sceneMtime = st.st_mtime;
    }
    while (ctx->running) frame(ctx);

    renderer.shutdown();
    SDL_GL_DeleteContext(glCtx);
    SDL_DestroyWindow(window);
    SDL_Quit();
    delete ctx->inputSrc;
    delete ctx->sim;
    delete scene;
    delete ctx;
#endif
    return 0;
}
