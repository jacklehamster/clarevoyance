# Clarevoyance — Engine Architecture

## Overview

The engine is a state-driven, single-draw-call sprite renderer built on C++17, SDL2, and OpenGL 3.3 Core. Game logic never touches OpenGL directly — it hands the renderer a `WorldState` or `Diff` and the engine handles everything from there.

The central design principle: **per-instance attributes encode motion and animation as functions of time**. The GPU evaluates position and frame index from a global `uTime` uniform, so the instance buffer only needs an update when a trajectory or animation state actually *changes* — not every frame.

---

## Layer Separation

```
┌─────────────────────────────────────┐
│  Game / Demo  (src/engine/main.cpp) │  builds WorldState / Diff
├─────────────────────────────────────┤
│  Renderer API                       │  applyState(), applyDiff(), render()
├─────────────────────────────────────┤
│  GPU  (vertex + fragment shaders)   │  motion, animation, billboarding
└─────────────────────────────────────┘
```

Game logic never calls a GL function. The `WorldState` / `Diff` structs are the only seam between layers.

---

## The Instance Model

Every renderable sprite is an `Instance` — a POD struct that is the CPU mirror of the per-instance GPU attribute layout:

```cpp
struct Instance {
    Vec3  pos;          // world position at motionStart
    Vec3  vel;          // velocity (units/sec)
    Vec3  accel;        // acceleration (e.g. gravity)
    float motionStart;  // time origin for motion formula

    Vec2  scale;        // world-space sprite size
    float rotation;     // yaw about world-up (oriented sprites)
    float billboard;    // 1 = always face camera

    Vec4  anim;         // firstFrame, frameCount, fps, animStart

    Vec4  tint;         // RGBA multiplier applied in the fragment shader
                        // (1,1,1,1 = opaque unmodified; alpha < 1 = translucent shim)

    float sheet;        // sprite-sheet index = texture-array layer to sample
                        // (value returned by Renderer::loadSheet; default 0)
};
```

The GPU evolves each instance every vertex invocation:

```glsl
float dt     = max(0.0, uTime - iMotionStart);
vec3  center = iPos + iVel * dt + 0.5 * iAccel * dt * dt;

float animDt = max(0.0, uTime - iAnim.w);
float frame  = iAnim.x + mod(floor(animDt * iAnim.z), iAnim.y);
```

A thrown projectile flying through an arc uploads its instance once. The GPU draws the correct position at every frame until a collision event triggers a new `Diff` with updated trajectory parameters.

---

## Rendering Pipeline

### One draw call

All sprites share a single unit quad (two triangles, six indices). The quad is drawn `N` times via `glDrawElementsInstanced`. Per-instance attributes are bound with `glVertexAttribDivisor(loc, 1)`.

```
Quad VBO  ──┐
Quad EBO  ──┤──► glDrawElementsInstanced(GL_TRIANGLES, 6, …, N)
Instance  ──┘
VBO (N×Instance)
```

### Sprite sheets — one texture array, loadable at runtime

All sprite sheets live in a single `GL_TEXTURE_2D_ARRAY` (`Renderer::MAX_SHEETS = 16`
square layers of `SHEET_LAYER_SIZE = 1024`). `Renderer::init()` takes no sheet —
sheets are runtime resources:

```cpp
bool init();                                            // GL objects only
int  loadSheet(const char* path, int cols, int rows);   // → sheet index (layer)
```

`loadSheet` is callable any time after init (including between scenes) and is
idempotent — reloading the same `(path, cols, rows)` returns the existing index, so
scene hot-reload never leaks layers. The loader repacks the source PNG's cells
row-major into the layer (flat cell order — and thus every animation frame index —
is preserved; only the wrap width changes), so sheets of any shape fit as long as
individual cells do. `Instance::sheet` selects the layer per instance; because the
one array is bound once, multiple sheets add **zero** texture binds or draw calls.

### Sprite sheet animation

Per-layer cell layout is passed as `uniform vec4 uSheetGrid[MAX_SHEETS]`
(x = columns per row after repacking, yz = cell size in UV units). The vertex
shader maps a flat cell index to a UV sub-rectangle and forwards the layer index
(`flat`) to the fragment shader, which samples the `sampler2DArray`:

```glsl
vec4 grid = uSheetGrid[int(iSheet + 0.5)];
float col = mod(cell, grid.x);
float row = floor(cell / grid.x);
vUV = (vec2(col, row) + aCornerUV) * grid.yz;
// fragment: texture(uTex, vec3(vUV, vSheet))
```

No extra draw calls per animation frame. No CPU involvement between uploads.

### Billboarding vs. oriented sprites

Controlled per instance by the `billboard` float flag:

- **Billboard (`billboard = 1`):** quad is offset along camera right/up vectors (passed as uniforms). Always faces the camera regardless of rotation.
- **Oriented (`billboard = 0`):** quad is offset along a world-space right axis derived from `rotation` (yaw about Y). Useful for floor decals, placed objects.

### Alpha cutout

The fragment shader discards fragments with `alpha < 0.05`, giving clean sprite outlines with no blending overhead.

---

## State Management

### WorldState

Full snapshot. Replaces everything in the renderer.

```cpp
struct WorldState {
    std::unordered_map<EntityId, Instance> instances;
    std::vector<Camera> cameras;
    int activeCamera = 0;
};
```

### Diff

Incremental change. Only touches what changed.

```cpp
struct Diff {
    std::vector<std::pair<EntityId, Instance>> upserts;
    std::vector<EntityId> removals;
    bool replaceCameras   = false;
    std::vector<Camera>   cameras;
    bool setActiveCamera  = false;
    int  activeCamera     = 0;
};
```

The renderer maintains a packed `vector<Instance>` for GPU upload plus an `id → slot` map for O(1) lookup. Removal uses swap-remove to keep the array packed without holes.

Instance buffer re-upload happens at most once per frame, only when `instancesDirty_` is set.

---

## Camera System

`Camera` supports two projection modes:

| Field | Perspective | Orthographic |
|-------|-------------|--------------|
| `fovY` | used | ignored |
| `orthoHalfHeight` | ignored | used |
| `aspect` | set by renderer from viewport | set by renderer |
| `near` / `far` | used | used |

`viewProjection()` returns a column-major `Mat4` passed directly to `glUniformMatrix4fv`. Camera `right()` and `trueUp()` vectors are extracted and passed as uniforms for billboarding.

Multiple cameras can live in `WorldState`. `activeCamera` index selects which one renders. Switching cameras costs one uniform update — no re-upload.

---

## Fixed-Timestep Simulation Loop

The game simulation advances in fixed ticks of `SIM_DT = 1/60 s` (60 Hz — owner
decision, see `docs/specs/SHIM_SYSTEM.md`). Sim time is **derived** from an integer
tick counter (`SimClock`, `src/game/sim.h`), never stored and never read from the
wall clock, so it cannot drift and the same input sequence produces the same
trigger firings at any frame rate on desktop, WASM, and Switch.

### Accumulator

The engine loop (`main.cpp`) runs a classic accumulator:

```
accum += wallDt  (capped at 0.25 s — no spiral of death after a pause)
while (accum >= SIM_DT):
    step the sim exactly one tick at simTime = clock.time()
    accum -= SIM_DT
render(clock.time() + accum)   // continuous time
```

Each sim tick runs input resolution, `applyMovement`, and event evaluation at the
tick's sim time — see `stepSim(const Scene&, SimState&, const ResolvedActions&,
Diff&)` in `src/game/sim.{h,cpp}`, which is pure with respect to its arguments.

### Render interpolation

`renderer.render()` receives the **continuous** time `clock.time() + accum` — sim
time plus the unsimulated remainder — so visuals never stutter at frame rates that
don't divide 60 Hz. Because motion is the analytic formula `pos + vel·t + ½·accel·t²`
evaluated on the GPU, this is *exact*, not an approximation.

### Input: engine-built, tick-stamped

The game layer is **SDL-free**. The engine translates SDL events into a
device-agnostic `InputFrame` (lowercase key names, down/pressed/released sets)
once per rendered frame via `buildInputFrame` (`src/engine/sdl_input.{h,cpp}`) —
the only place SDL keyboard events meet game input. The frame merges into a
pending tick command; the first sim tick of the frame consumes the
pressed/released edges (they never repeat across ticks; down-state persists), and
edges arriving on a frame that runs zero ticks are kept for the next tick, never
lost. Resolving actions for a tick is a pure function of (command, bindings) —
conceptually a tick-stamped command, ready for lockstep networking.

### Immutable Scene, copyable SimState

After `loadScene()` the `Scene` is immutable data. ALL runtime mutation lives in
`SimState` (`src/game/sim.h`): entity instances, per-entity attrs, flags, per-event
fired markers, and the `SimClock`. `SimState` is cheaply copyable with no pointers
into the Scene or renderer — copying it and stepping the copy forward is the shim
lookahead's core operation.

### CV_FIXED_TIME

In fixed-time test mode the clock is pinned at `tick = round(CV_FIXED_TIME /
SIM_DT)` and one sim step runs per rendered frame at that constant sim time, so
test screenshots stay deterministic across runs and platforms.

---

## Clairvoyance Shim System (planned)

The shim system — ghost previews of future entity positions — maps cleanly onto this architecture:

1. The CPU runs a lightweight lookahead simulation N seconds ahead (positions + AABB collision, no rendering).
2. Shim instances are added to the scene with the *future* position encoded as `pos` and `motionStart = now + lookahead`.
3. They are rendered with a translucent tint via the `tint` field on Instance (implemented — per-instance GPU attribute at location 10, multiplied in the fragment shader).

Because the simulation is deterministic (same inputs → same outputs, seeded RNG only), the lookahead produces stable, flicker-free shims. The GPU evaluates them using the exact same motion formula as live sprites.

Both prerequisites are now in place: the fixed-timestep sim (60 Hz tick clock) and the forkable `SimState` — copy it, advance the copy with `stepSim`, read out predicted positions, discard (see the Fixed-Timestep Simulation Loop section).

---

## Determinism

All game simulation must be deterministic from day one — required by the shim lookahead system.

- Fixed 60 Hz timestep; sim time = `tick * SIM_DT`, never wall clock (see the
  Fixed-Timestep Simulation Loop section). No frame-rate-dependent physics. All
  motion uses the explicit time formula above.
- Seeded RNG only, never `rand()`.
- Enemy AI state transitions must be functions of simulation time and input events, not wall-clock time.
- Enforced by `make test-unit`: a determinism replay test runs the same
  tick-stamped input script under different frame batchings and asserts
  bit-identical results.

The GPU side is inherently deterministic: same `uTime` + same instance buffer → same pixels.

---

## File Map

```
src/engine/
  gl.h              — platform GL include (Desktop / GLES3 / Emscripten stub)
  mathx.h           — header-only vec2/3/4, Mat4, perspective, ortho, lookAt
  shader.h/.cpp     — compileShader, buildProgram, setUniform overloads
  texture.h/.cpp    — TextureArray: one GL_TEXTURE_2D_ARRAY for all sprite
                      sheets, cells repacked per layer (GL_NEAREST, stb_image)
  camera.h          — Camera struct, viewProjection(), right(), trueUp()
  instance.h        — Instance POD + makeBillboard/makeSprite/setAnimation/setMotion
  world_state.h     — WorldState, Diff, EntityId
  renderer.h/.cpp   — Renderer: owns VAO/VBOs/program/texture array,
                      init/loadSheet/applyState/applyDiff/render
  sdl_input.h/.cpp  — buildInputFrame: SDL events → device-agnostic InputFrame
                      (the ONLY place SDL keyboard events meet game input)
  main.cpp          — entry point: fixed-timestep accumulator loop; stress-test
                      demo, or a data-driven scene via CV_SCENE=path (with
                      once-per-second hot-reload on desktop)
  screenshot.h/.cpp — captureFramebufferBase64, framebufferNonBlank (test helpers)
  third_party/
    stb_image.h       — vendored v2.30
    stb_image_write.h — vendored (used by screenshot.cpp for PNG encoding)

src/game/           — SDL-free; compiles without GL (see make test-unit)
  json.h            — minimal JSON parser for scene files (no external deps)
  scene.h/.cpp      — Scene (immutable after load): entities/cameras/events/
                      bindings/attrs parsed from JSON, loadScene(), applyMovement
  events.h/.cpp     — updateEvents: data-driven trigger/condition/action rules → Diff
  input.h/.cpp      — InputFrame/Bindings/ResolvedActions, InputSource
                      (BindingsInputSource holds a COPY of the bindings)
  sim.h/.cpp        — SIM_DT, SimClock, copyable SimState, stepSim (one tick)

src/levels/
  *.json            — canonical scene files (demo, controls, menu)

tests/
  *.cpp             — zero-dependency unit tests (make test-unit): json parser,
                      event semantics, determinism replay, scene loading

tools/
  imgdiff.c         — native pixel-diff tool for desktop/web parity checks

scripts/
  extract_shot.sh   — pulls base64 PNG from between CV_SHOT_BEGIN/CV_SHOT_END markers

web/
  pre.js            — Emscripten preRun hook (URL params → ENV, kept for reference)
```

---

## Stress Test Results

Single draw call, instance buffer uploaded once at startup, animation entirely on GPU:

| Grid | Sprites | FPS |
|------|---------|-----|
| 10×10 | 100 | 65 |
| 30×30 | 900 | 56 |
| 100×100 | 10,000 | 60 (vsync) |
| 300×300 | 90,000 | 60 (vsync) |
| 1000×1000 | 1,000,000 | 60 (vsync) |

GPU is not the bottleneck at any tested count. The 1M case costs ~90ms on initial buffer upload (72MB), then locks to vsync.

---

## Cross-Platform Build

The same C++ source compiles to desktop (OpenGL 3.3 Core) and browser (WebGL 2 / GLES 3.0) via Emscripten. Four seams isolate all platform differences:

### 1. GL header — `src/engine/gl.h`

Already branches correctly: desktop includes `<GL/gl.h>` (macOS: `<OpenGL/gl3.h>`), Emscripten includes `<GLES3/gl3.h>`. No change needed here — just don't bypass it.

### 2. GL context attributes — `src/engine/main.cpp`

WebGL 2 requires a GLES 3.0 context (minor version 0, profile ES). Desktop needs 3.3 Core. Wrapped with `#ifdef __EMSCRIPTEN__`.

**Rationale:** GLES 3.0 is a well-defined subset of OpenGL 3.3 Core. Everything the renderer uses — VAOs, `glDrawElementsInstanced`, `glVertexAttribDivisor`, integer uniforms, user fragment outputs — is core in GLES 3.0. No feature guards needed in engine code.

### 3. GLSL version/precision header — `src/engine/shader.cpp`

GLSL 3.30 Core and GLSL ES 3.00 differ only in the version directive and a missing default float precision in ES fragment shaders. The shader bodies are identical.

`shaderHeader(GLenum type)` returns the correct preamble per platform and stage:
```cpp
// Desktop:   "#version 330 core\n"
// WASM vert: "#version 300 es\n"
// WASM frag: "#version 300 es\nprecision highp float;\nprecision highp int;\n"
```

`glShaderSource` is called with a two-element array `{header, source}` so `#version` is always the first token. Shader bodies in `renderer.cpp` contain no version directive.

### 4. Main-loop wrapper — `src/engine/main.cpp`

The browser owns the event loop; WASM cannot block. The loop body lives in a `frame(void*)` function:
- **Desktop:** driven by a plain `while (ctx.running) frame(&ctx)`.
- **WASM:** driven by `emscripten_set_main_loop_arg(frame, ctx, fps, 1)`.
  - `fps = 0` → `requestAnimationFrame` (display-synced; pauses when the tab is in the background) — used for interactive mode.
  - `fps = 60` → `setTimeout` at 60 Hz (fires even in background/hidden tabs) — used for test mode. **Critical:** `rAF` is suspended by Chrome for non-active tabs, so test mode must use `setTimeout`.
  - `simulate_infinite_loop = 1`: `main()` never returns; `exit(0)` terminates and signals `emrun` with `EXIT_STATUS:0`. Required with `EXIT_RUNTIME=1 + --emrun`.

### Makefile

`WASM_FLAGS` key flags:
- `-sUSE_SDL=2 -sUSE_WEBGL2=1 -sFULL_ES3=1` — Emscripten SDL2 port + WebGL 2 mapping.
- `-sEXIT_RUNTIME=1` — allows `exit()` / atexit handlers to work in WASM.
- `--emrun` — compiles in stdout forwarding to the emrun HTTP server (required for `exit()` to signal `emrun`; incompatible with `EXIT_RUNTIME=0`).
- `--preload-file art` — packages `art/` into `stress.data`; virtual path `art/penguin.png` is identical to desktop, so no path changes in engine code.
- `--preload-file src/levels@scenes` — maps the canonical scene directory into the virtual FS as `scenes/`, so web URLs use `scenes/...` paths.
- Outputs are `build/web/stress.{html,js,wasm,data}`.

---

## Testing

### Architecture

All test config is read via `getenv()` in C++. Platform differences are transparent to the test code:
- **Desktop:** real shell env vars (`CV_TEST_FRAMES=120 CV_FIXED_TIME=2.0 CV_SCREENSHOT=1 ./clarevoyance`).
- **WASM:** URL query params read directly via `EM_ASM` + `URLSearchParams` in `main.cpp`. (`ENV` injection via `pre.js` exists but was unreliable under Emscripten 6.0 closure scoping.)

### Config keys

| Key | Effect |
|-----|--------|
| `CV_TEST_FRAMES=N` | Render N frames then exit 0. Unset → run forever. |
| `CV_FIXED_TIME=T` | Pin the sim clock at `tick = round(T / SIM_DT)` and render at constant `t = T` every frame instead of wall clock. Makes output deterministic across runs and platforms. |
| `CV_SCREENSHOT=1` | On the final test frame, capture the back buffer as a base64 PNG between `CV_SHOT_BEGIN` / `CV_SHOT_END` markers on stdout. |

### Unit tests — `make test-unit`

`tests/*.cpp` + `src/game/*.cpp` compile into `build/test_unit` — pure CPU, no
GL/SDL, no window. Covers the JSON parser, event trigger/condition/action
semantics, the determinism replay (identical tick-command scripts under
different frame batchings must be bit-identical), and loading every scene in
`src/levels/`. Runs in CI before the compile gate.

### Screenshot capture — `src/engine/screenshot.{h,cpp}`

- `captureFramebufferBase64(int w, int h)`: `glReadPixels` → vertical flip → `stbi_write_png_to_func` → base64 → `printf` between markers. Identical on both platforms.
- `framebufferNonBlank(int w, int h)`: counts pixels differing from clear color; logs `CV_BLANK` if below 1% threshold (catches blank-screen regressions automatically).
- `scripts/extract_shot.sh <log> <out.png>`: extracts the first base64 block from the log and decodes it. Used by both `make test` and `make test-wasm`.

### Parity diff — `tools/imgdiff.c`

Native C tool (no new deps, built with `clang`). Reads two PNGs via `stb_image`, asserts equal dimensions, reports mean absolute per-channel difference (0–255) and % of pixels exceeding a per-pixel epsilon. Exits 0 if mean ≤ tolerance, else 1.

Current baseline: **0.00/255 mean diff, 0.0% pixels differ** — desktop (OpenGL 3.3 Core / Metal) and web (WebGL 2 / ANGLE) render pixel-identically at fixed time.

### emrun integration

`emrun --kill-exit --timeout 30 "stress.html?..."` launches Chrome, captures stdout via HTTP POST (the `--emrun`-compiled binary POSTs each line), and terminates when the WASM calls `exit(0)`. `--kill-exit` closes the browser process; `--timeout 30` kills everything if the test hangs.

### one-shot guard

`exit(0)` in Emscripten does not immediately stop the loop — more frame callbacks fire during cleanup. `LoopContext::testDone` is a boolean that ensures the screenshot/exit block runs exactly once.

---

## Hard Constraints

These are non-negotiable — see [CLAUDE.md](../CLAUDE.md) for full rationale.

- Third-person camera; Clare always visible on screen.
- No minimap.
- Camera rotation locked to 90° increments; character/player movement is free (not grid-locked).
- Shim system is a 3D world effect, not a HUD element.
- Shader-based frame animation — animation state never moves to CPU.
- Game simulation must remain fully deterministic.
- Engine and game logic stay in separate layers.
