# Clarevoyance — Game Layer Architecture

This document covers everything *above* the engine API. For the renderer contract
(`Instance`, `WorldState`, `Diff`, `Camera`), see [ARCHITECTURE.md](ARCHITECTURE.md).
The game layer builds `Diff`s and hands them to the renderer — it never calls GL directly.

---

## Roadmap specs

Requirements documents for the next major systems live in `docs/specs/`:

- [SCENE_COORDINATOR.md](specs/SCENE_COORDINATOR.md) — runtime scene switching, `change_scene` + spawn points, persistent GameState vs scene-local state.
- [WORLD_BUILDING.md](specs/WORLD_BUILDING.md) — floor/wall tile layer, spatial tile cache, roads with forks, collision, landmark navigation.
- [MAP_EDITOR.md](specs/MAP_EDITOR.md) — strict versioned scene schema, JSON round-trip serializer, archetypes and named clips, editor tooling.
- [SHIM_SYSTEM.md](specs/SHIM_SYSTEM.md) — fixed-timestep deterministic sim, lookahead fork, translucent shim render pass, upgrade tiers, corruption effects.

---

## Responsibilities

| Layer | Owns |
|-------|------|
| Engine (`src/engine/`) | Rendering, camera, billboarding, shader animation, shim visual pass |
| Game (`src/game/`) | Combat, enemy AI, clairvoyance logic, event system, world/level loading |

---

## Core Gameplay Loop

The sim advances in fixed **60 Hz ticks** (`SIM_DT = 1/60 s`, `src/game/sim.h`);
the engine loop runs an accumulator over wall time (see ARCHITECTURE.md,
Fixed-Timestep Simulation Loop). Per rendered frame:

```
1. Engine translates SDL events → InputFrame (once per frame; game layer is SDL-free)
2. accumulator += wall dt (capped);  while accumulator >= SIM_DT:
     a. Resolve the tick-stamped input command → ResolvedActions (pure function;
        pressed/released edges are consumed by the first tick, never repeated)
     b. stepSim(scene, simState, actions, diff):
          - applyMovement to controlled entities
          - evaluate events (triggers/conditions/actions)
          - upsert entities whose trajectory/animation changed, remove despawns
          - simState.clock.tick++
     c. accumulator -= SIM_DT
3. (future) Run clairvoyance lookahead: copy SimState, step the copy N/SIM_DT
   ticks, upsert shim ghosts from the result
4. renderer.applyDiff(diff)
5. renderer.render(clock.time() + accumulator)   ← continuous time, smooth motion
```

Steps 1–3 are pure CPU simulation over an immutable `Scene` plus a copyable
`SimState` (all runtime state: entities, attrs, flags, fired markers, clock).
Steps 4–5 are engine calls. The game layer never touches a VBO or uniform
directly — and never sees an SDL type.

---

## Entity Model

Each game entity maps to one or more `Instance`s in the renderer. The game layer owns
entity identity and state; the renderer owns the visual representation.

```
Entity (game layer)
  ├── EntityId  → live Instance  (current sprite)
  └── EntityId  → shim Instance  (clairvoyance ghost, if visible)
```

Entity IDs are assigned by the game layer. Odd IDs = live sprites; the paired even ID
= shim ghost — or use a separate ID namespace. TBD during implementation.

---

## Clairvoyance Shim System

### Concept

Shims are translucent ghost sprites that show where enemies will be N seconds from now.
They are normal `Instance`s rendered with reduced opacity (future: `tint` field on
`Instance`, or a second draw pass with alpha uniform). The engine renders them; the
game layer computes them.

### Lookahead Simulation

The game runs a simplified forward simulation N seconds ahead:

- Inputs: current entity positions, velocities, AI state machines (no player collision)
- Outputs: predicted positions and animation states at `now + N`
- Collision detection: AABB only, enough to know when an attack arc terminates

Because the simulation is fully deterministic, the lookahead produces stable,
non-flickering shims. Same sim state + same N → same ghost positions every frame.

### Encoding Shims as Instances

A shim is an Instance whose motion is set to the *predicted* future trajectory:

```cpp
// Live entity at current position
Instance live = makeBillboard(currentPos, scale);
setAnimation(live, walkFirst, walkCount, fps, animStart);

// Shim: same trajectory but motionStart is in the future
Instance shim = live;
shim.motionStart = now + lookaheadSeconds;
// tint = translucent (TBD)
```

The GPU evaluates both using the identical motion formula. No second render path needed.

### Clare's Upgrades

Upgrades expand `lookaheadSeconds` and improve shim clarity (opacity, detail level).
This is the entire RPG progression system — every upgrade changes how the game *feels*.

| Upgrade tier | Lookahead | Effect |
|-------------|-----------|--------|
| 0 (base) | 0.5s | faint outlines only |
| 1 | 1.0s | clearer silhouettes |
| 2 | 2.0s | full colour ghosts |
| 3 | 3.5s | attack arc previewed |

Exact values TBD during tuning.

---

## Enemy AI

### Requirements

- All AI must be **deterministic**: state transitions are functions of sim time and
  observable game events only. No `rand()` without a seeded RNG.
- AI is defined in data files where possible (see Event System below).
- Complex bosses (Hiri, King Dev) may have hand-coded C++ AI modules in `src/game/ai/`.

### State Machine Pattern

Each enemy has a simple state machine. Transitions fire at sim-time events:

```
Idle → Approach (when player enters radius)
     → Attack   (when in range, cooldown elapsed)
     → Retreat  (when health < threshold)
Attack → Wind-up → Strike → Recovery → Idle
```

Wind-up and Strike phases are what the shim system previews. The lookahead sim only
needs to advance AI through these phases — it doesn't need full AI fidelity.

### Notable Enemies

See [STORY.md](STORY.md) for character details. AI notes:

- **Spirolo** — extremely fast; slowed by water. Water tiles modify movement speed constant.
- **Twin Dolls** — perfectly mirrored attack pattern. Dodging one leads into the other.
  Implemented as two AI agents sharing a mirrored state machine.
- **Amblico** — corrupts Clare's clairvoyance. His presence causes shims to show *false*
  positions. Implemented by injecting noise into the lookahead output, not the sim itself.
- **King Dev** — no magic; psychologically manipulates Clare's shims. Fakes wind-ups,
  speaks during combat. AI reads Clare's dodge history to find exploitable patterns.

---

## Script Layer (Scene Files)

Game content is defined in **JSON scene files** (`src/levels/*.json`), parsed by the game
layer into a `WorldState` plus a list of events. The engine is unchanged — it just renders
the `WorldState` and applies the `Diff`s the event system emits. JSON was chosen over an
embedded scripting language so the simulation stays trivially **deterministic** (data, not
executed code) and identical across desktop / WASM / Switch.

Implementation: `src/game/json.h` (parser), `src/game/scene.{h,cpp}` (loader),
`src/game/events.{h,cpp}` (runtime). Run a scene with the `CV_SCENE` env var (desktop) or
`?CV_SCENE=` query param (web).

```bash
make demo                          # runs src/levels/demo.json in a window
make demo SCENE=src/levels/foo.json   # run a different scene
```

In the demo, the player penguin walks toward Mochi; when it gets within range a proximity
event fires — printing `CV_DIALOGUE: mochi_intro`, flipping Mochi to a surprised frame, and
sending Mochi leaping away via `set_motion`.

### Scene schema

```json
{
  "sheet": { "path": "art/penguin.png", "cols": 16, "rows": 1 },
  "cameras": [
    { "projection": "perspective", "position": [6, 7, 16], "target": [6, 0, 5] }
  ],
  "activeCamera": 0,
  "entities": [
    { "id": "player", "pos": [0, 0.45, 5], "scale": [0.9, 0.9], "billboard": true,
      "vel": [1.2, 0, 0], "anim": { "first": 0, "count": 5, "fps": 10 } },
    { "id": "mochi",  "pos": [11, 0.45, 5], "scale": [0.9, 0.9], "billboard": true,
      "anim": { "first": 7, "count": 1, "fps": 0 } }
  ],
  "events": [ /* see below */ ]
}
```

Entities are referenced by string `id` in the data file; the loader assigns numeric
`EntityId`s (starting at 1) and keeps the name→id map. Optional fields: `vel`, `accel`
(motion, evaluated on the GPU and mirrored on the CPU for triggers), `rotation` (non-billboard).

### Event / Condition / Action system

Each event is a **trigger** + an optional **condition** + a list of **actions**, evaluated
every simulation step. Triggers are pure functions of sim state (entity positions, flags) —
no wall-clock, no `rand()` — so they fire deterministically.

```json
{
  "trigger": { "type": "proximity", "entity": "player", "target": "mochi", "radius": 1.5 },
  "condition": { "flag": "mochi_seen", "value": false },
  "once": true,
  "actions": [
    { "type": "dialogue", "id": "mochi_intro" },
    { "type": "set_flag", "flag": "mochi_seen", "value": true },
    { "type": "set_anim", "entity": "mochi", "first": 6, "count": 1, "fps": 0 }
  ]
}
```

| Trigger     | Fields                          | Fires when |
|-------------|---------------------------------|------------|
| `start`     | —                               | first step (subject to condition) |
| `proximity` | `entity`, `target`, `radius`    | distance(entity, target) ≤ radius |
| `input`     | `action`, `edge`                | the abstract `action` shows `edge` this tick (`pressed`/`released`/`held`; default `pressed`) — see Controls / Input |

| Action       | Fields                                  | Effect |
|--------------|-----------------------------------------|--------|
| `dialogue`   | `id`                                    | emits a `CV_DIALOGUE: <id>` line (UI sink TBD) |
| `set_flag`   | `flag`, `value`                         | sets a boolean flag |
| `set_anim`   | `entity`, `first`, `count`, `fps`       | swaps an entity's animation (emits an upsert `Diff`) |
| `set_motion` | `entity`, `vel`, `accel`                | rebases motion from the entity's current position and gives it a new velocity/acceleration (e.g. an enemy fleeing or a thrown arc) |
| `remove`     | `entity`                                | despawns an entity (emits a removal `Diff`) |
| `toggle_controlled` | `entity`                         | flips the entity's `controlled` attribute (adds/removes it from the controlled set) |
| `set_controlled`    | `entity`, `value`                | sets the entity's `controlled` attribute to `value` |

`once` (default true) fires the event at most once. Events run from the simulation step and
produce `Diff`s the renderer applies — they never call GL. This is the same seam the
clairvoyance lookahead uses, so shim ghosts and scripted events share one deterministic sim.

---

## Controls / Input

Keyboard input is a small, device-agnostic layer (`src/game/input.{h,cpp}`) that sits between
the engine and the game logic. It has three pieces: an **InputFrame** (raw per-frame key state),
the **Bindings** (key → abstract action), and the **movement** pass that drives controlled
entities (`applyMovement`, declared in `scene.h`, run per tick by `stepSim`). The game layer is
**SDL-free**: the ENGINE builds the InputFrame from SDL events (`src/engine/sdl_input.{h,cpp}`,
`buildInputFrame`) once per rendered frame; the events demo path is unaffected (no bindings).

**Character/player movement is free — not grid-locked.** The 90° lock is a *camera* constraint
(see [CLAUDE.md](CLAUDE.md)), not a movement one.

### Abstract action model

The engine never hands SDL keycodes to game logic. Each frame it builds an `InputFrame` of
lowercase, device-agnostic key names — letters as-is (`"w"`), arrows as `"up"`/`"down"`/
`"left"`/`"right"`, space as `"space"` — split into three sets: keys currently `down`, keys
`pressed` this frame, and keys `released` this frame. Bindings then resolve those keys into
**abstract actions**, so game data talks about intent (`move_north`) rather than hardware.

### Bindings schema

A scene may declare a `controls` block mapping key names to abstract action names:

```json
"controls": {
  "bindings": {
    "w": "move_north", "a": "move_west", "s": "move_south", "d": "move_east",
    "up": "move_north", "left": "move_west", "down": "move_south", "right": "move_east",
    "space": "toggle_extra"
  }
}
```

Multiple keys may map to the same action (WASD *and* arrows above). Unbound keys contribute
nothing. The bindings are parsed onto the `Scene`; the active input source
(`BindingsInputSource`) holds a **copy** of them — so it stays valid across scene
unload/hot-reload — and resolves each tick's `InputFrame` into down/pressed/released
**action** sets via `ResolvedActions poll(const InputFrame&)`. Edges (pressed/released) are
consumed by the first sim tick of the frame and never repeat on later ticks; held state
persists. Resolution is a pure function — the per-tick input is effectively a tick-stamped
command, ready for lockstep replay/networking.

### Entity attributes: `controlled` and `speed`

Movement is driven by per-entity, game-side attributes — kept separate from the GPU `Instance`
(which must stay a standard-layout block of floats). They live in an `EntityAttrs` store inside
the copyable `SimState` (`src/game/sim.h`), initialised from the immutable scene:

| Field        | Default | Meaning |
|--------------|---------|---------|
| `controlled` | `false` | movement input applies to *every* entity whose `controlled` is true |
| `speed`      | `3.0`   | movement rate in world units / second |

Both are optional entity JSON fields:

```json
{ "id": "player", "pos": [0, 0.9, 0], "controlled": true, "speed": 3.0, ... }
```

`controlled` is a per-entity attribute, **not** a global "who is active" flag — you can control
one entity, several at once, or toggle membership at runtime (via `toggle_controlled` /
`set_controlled`). This is what lets the controls demo move the player alone, then add a buddy.

### Movement model

The directional actions `move_north` (−Z), `move_south` (+Z), `move_west` (−X), `move_east` (+X)
follow the engine coordinate convention (+X east, +Z south, +Y up). Held directions sum into a
world-space vector, normalised if non-zero (so diagonals aren't faster), then scaled by each
controlled entity's `speed` and applied as **velocity** — using the same rebase-to-current-
position + `motionStart = now` approach as `set_motion`, at the tick's sim time. An upsert is
emitted only when an entity's desired velocity actually differs from its current velocity, so a
stationary or steady-moving entity produces no per-tick `Diff` churn.

### `input` trigger

The `input` trigger fires when an abstract action shows a given edge this frame:

```json
{ "trigger": { "type": "input", "action": "toggle_extra", "edge": "pressed" }, "once": false,
  "actions": [ { "type": "toggle_controlled", "entity": "buddy" } ] }
```

`edge` is one of `pressed` / `released` / `held` (default `pressed`). The resolved action sets
are threaded into `updateEvents` via `stepSim` each tick; a scene without bindings (like the
events demo) simply runs with empty action sets.

### Demo

```bash
make demo-controls   # src/levels/controls.json
make demo-events     # src/levels/demo.json (the proximity/dialogue demo)
```

In `demo-controls`, WASD or the arrow keys move the player penguin freely. Pressing **space**
fires an `input` trigger that toggles a second "buddy" penguin into the controlled set, so both
move together; pressing space again drops the buddy back out.

---

## World Structure

### Outdoors

- Long linear roads with forks. Forks lead to distinct areas; the primary path is always
  forward.
- No minimap. Navigation by landmark.
- Road segments are loaded from data files; the spatial cache (engine) loads/unloads tiles
  as the player moves.

### Dungeons

- More complex layouts. Third-person camera maintained throughout.
- Large open rooms may trigger overhead/isometric camera view (automatic or manual — TBD).
- Camera switch is a `Diff` that updates `activeCamera` index.

---

## Coordinate Conventions

Follows the engine's right-handed coordinate system:

- **+X** east, **+Z** south, **+Y** up
- Grid cells are 1×1 world units
- Character/player movement is free (not grid-locked); camera lerps smoothly between positions
- Camera rotation is locked to 90° increments (0°, 90°, 180°, 270°)

---

## Doc Sync Rule

Any change to `Instance`, `WorldState`, `Diff`, or `Camera` in the engine must update
[ARCHITECTURE.md](ARCHITECTURE.md) first. This doc references those contracts — it does
not duplicate field definitions. If the two docs contradict each other, ARCHITECTURE.md
is authoritative.
