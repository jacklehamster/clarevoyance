# Clarevoyance — Game Layer Architecture

This document covers everything *above* the engine API. For the renderer contract
(`Instance`, `WorldState`, `Diff`, `Camera`), see [ARCHITECTURE.md](ARCHITECTURE.md).
The game layer builds `Diff`s and hands them to the renderer — it never calls GL directly.

---

## Responsibilities

| Layer | Owns |
|-------|------|
| Engine (`src/engine/`) | Rendering, camera, billboarding, shader animation, shim visual pass |
| Game (`src/game/`) | Combat, enemy AI, clairvoyance logic, event system, world/level loading |

---

## Core Gameplay Loop

```
1. Read player input
2. Advance simulation by dt (deterministic — no wall-clock dependency)
3. Detect collisions / trigger events
4. Run clairvoyance lookahead (N seconds ahead of current sim time)
5. Build Diff:
     - upsert entities whose trajectory/animation changed
     - upsert shim ghosts from lookahead result
     - remove entities that left the scene
6. renderer.applyDiff(diff)
7. renderer.render(simTime)
```

Steps 1–5 are pure CPU simulation. Steps 6–7 are engine calls. The game layer never
touches a VBO or uniform directly.

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

| Action       | Fields                                  | Effect |
|--------------|-----------------------------------------|--------|
| `dialogue`   | `id`                                    | emits a `CV_DIALOGUE: <id>` line (UI sink TBD) |
| `set_flag`   | `flag`, `value`                         | sets a boolean flag |
| `set_anim`   | `entity`, `first`, `count`, `fps`       | swaps an entity's animation (emits an upsert `Diff`) |
| `set_motion` | `entity`, `vel`, `accel`                | rebases motion from the entity's current position and gives it a new velocity/acceleration (e.g. an enemy fleeing or a thrown arc) |
| `remove`     | `entity`                                | despawns an entity (emits a removal `Diff`) |

`once` (default true) fires the event at most once. Events run from the simulation step and
produce `Diff`s the renderer applies — they never call GL. This is the same seam the
clairvoyance lookahead uses, so shim ghosts and scripted events share one deterministic sim.

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
- Player movement snaps to grid; camera lerps smoothly between positions
- Rotation locked to 90° increments (0°, 90°, 180°, 270°)

---

## Doc Sync Rule

Any change to `Instance`, `WorldState`, `Diff`, or `Camera` in the engine must update
[ARCHITECTURE.md](ARCHITECTURE.md) first. This doc references those contracts — it does
not duplicate field definitions. If the two docs contradict each other, ARCHITECTURE.md
is authoritative.
