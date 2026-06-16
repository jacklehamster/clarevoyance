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
#include <string>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#include "renderer.h"
#include "screenshot.h"

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
};

static void frame(void* arg) {
    LoopContext* ctx = static_cast<LoopContext*>(arg);

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) ctx->running = false;
        if (event.type == SDL_KEYDOWN) {
            if (event.key.keysym.sym == SDLK_ESCAPE) ctx->running = false;
            if (event.key.keysym.sym == SDLK_c && event.key.repeat == 0)
                ctx->activeCam = (ctx->activeCam + 1) % 2;
        }
    }

    float t = ctx->fixedTime >= 0.0f
        ? ctx->fixedTime
        : (float)(SDL_GetTicks64() - ctx->startTicks) / 1000.0f;

    // Only thing that changes each frame: the camera orbit (uniforms only).
    Diff diff;
    diff.replaceCameras  = true;
    diff.cameras         = buildCameras(t * 0.1f);
    diff.setActiveCamera = true;
    diff.activeCamera    = ctx->activeCam;
    ctx->renderer->applyDiff(diff);

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

#ifdef __EMSCRIPTEN__
    // Read test params from URL query string directly — ENV scoping in
    // Emscripten 6.0 closures makes getenv() unreliable from pre.js.
    {
        char buf[64] = {};
        EM_ASM({
            var p = new URLSearchParams(location.search);
            var v;
            if ((v = p.get('CV_TEST_FRAMES'))) stringToUTF8(v, $0, 64);
            if ((v = p.get('CV_FIXED_TIME')))  stringToUTF8(v, $1, 64);
            if ((v = p.get('CV_SCREENSHOT')))  stringToUTF8(v, $2, 64);
        }, buf, buf + 16, buf + 32);
        if (buf[0])    testFrames   = std::atoi(buf);
        if (buf[16])   fixedTime    = std::atof(buf + 16);
        if (buf[32])   doScreenshot = (std::atoi(buf + 32) != 0);
        SDL_Log("Test mode: frames=%d fixedTime=%.1f screenshot=%d",
                testFrames, fixedTime, doScreenshot ? 1 : 0);
    }
#else
    if (const char* v = getenv("CV_TEST_FRAMES"))  testFrames   = std::atoi(v);
    if (const char* v = getenv("CV_FIXED_TIME"))   fixedTime    = std::atof(v);
    if (const char* v = getenv("CV_SCREENSHOT"))   doScreenshot = (std::atoi(v) != 0);
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
    SDL_Log("OpenGL: %s", glGetString(GL_VERSION));

    Renderer renderer;
    if (!renderer.init(SHEET_PATH, SHEET_COLS, SHEET_ROWS)) {
        SDL_Log("Renderer init failed — is %s present?", SHEET_PATH);
        SDL_GL_DeleteContext(glCtx); SDL_DestroyWindow(window); SDL_Quit();
        return 1;
    }
    renderer.setViewport(WINDOW_W, WINDOW_H);

    // --- Build the world once -----------------------------------------------
    WorldState state;
    state.cameras     = buildCameras(0.0f);
    state.activeCamera = 0;

    const int total = GRID_W * GRID_H;
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
    SDL_Log("Buffer uploaded. Rendering %d instances per frame.", total);

    // --- Run loop -----------------------------------------------------------
    Uint64 startTicks = SDL_GetTicks64();

#ifdef __EMSCRIPTEN__
    // Heap-allocate so the context outlives main() on the web.
    LoopContext* ctx = new LoopContext{
        window, &renderer,
        startTicks, startTicks,
        0, 0, total, true,
        testFrames, fixedTime, doScreenshot, 0, false
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
        testFrames, fixedTime, doScreenshot, 0, false
    };
    while (ctx.running) frame(&ctx);

    renderer.shutdown();
    SDL_GL_DeleteContext(glCtx);
    SDL_DestroyWindow(window);
    SDL_Quit();
#endif
    return 0;
}
