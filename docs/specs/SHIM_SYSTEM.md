# Spec: Clairvoyance Shim System

The core mechanic: translucent ghost previews ("shims") of enemy attacks N seconds
before they land, produced by a lookahead fork of the deterministic simulation.

Concept and gameplay intent live in
[GAME_LAYER.md](../GAME_LAYER.md#clairvoyance-shim-system); the engine-side sketch in
[ARCHITECTURE.md](../ARCHITECTURE.md#clairvoyance-shim-system-planned). This spec
turns both into testable requirements. Hard constraints: shims are a 3D world effect,
never HUD/UI.

---

## Purpose

The shim system is the game. Everything else (determinism, state separation,
translucent rendering) exists so that the sim can be forked, run N seconds ahead, and
rendered as ghosts in the present frame — and so that Clare's upgrades change how that
preview *feels*.

---

## Requirements

### Deterministic fixed-timestep simulation (prerequisite)

1. **MUST** move the simulation to a fixed timestep before any shim work: integer tick
   counter, `double` sim time derived as `tick * dt`, fixed `dt` = **1/60 s (60 Hz —
   decided by the owner)**. Rendering interpolates between sim states via the existing
   analytic motion model (`pos + vel*t + ½*accel*t²`) — the GPU already does this; the
   sim must match it.
2. **Blocker (ground truth):** sim time is currently wall-clock per frame; proximity
   and event triggers are frame-rate dependent. This **MUST** be fixed — same input
   sequence must produce the same trigger firings at any frame rate, on desktop, WASM,
   and Switch.
3. **MUST** use seeded RNG only, with every seed recorded; no `rand()`, no wall-clock
   reads inside the sim (already policy — see ARCHITECTURE.md Determinism).
3b. **Networked play IS planned (owner decision).** All inputs **MUST** enter the sim
   as tick-stamped commands (input at tick T applies at tick T), so the same command
   stream replays identically anywhere. The design **MUST** stay lockstep-friendly
   with input delay: a peer's commands for tick T can arrive up to the delay window
   late and still be applied deterministically.

### Sim state must be forkable (prerequisite)

4. **MUST** move all runtime state out of `Scene` into a copyable `SimState` (entity
   kinematics, AI state machines, flags/counters, event `fired` markers, RNG state).
   Scenes are immutable after load. **Blocker (ground truth):** `Event::fired`
   currently lives inside `Scene`, so the sim cannot be forked or replayed today.
5. **MUST** make `SimState` cheaply copyable (POD-ish, no owning pointers into
   scene/renderer) — the lookahead copies it every time the preview refreshes.

### Lookahead pass

6. **MUST** implement lookahead as: copy `SimState`, advance the copy `N / dt` ticks
   with simplified rules, read out predicted enemy positions/animation phases, discard
   the copy. The real sim is never touched.
7. **MUST** use simplified rules in the fork: no player collision, no player input
   (player assumed inert or continuing current velocity — see Open questions), AABB
   collision against walls only, AI advanced through wind-up/strike phases
   (GAME_LAYER.md state machine).
8. **MUST** be stable: with an unchanged real-sim state, consecutive frames produce
   identical shim trajectories (no flicker). Shims re-run only when a sim event
   invalidates the prediction.
9. **SHOULD** bound lookahead cost: budget target ≤ 1 ms/frame at max tier (3.5 s
   ahead); acceptable to refresh shims every K ticks rather than every tick.

### Rendering

10. **MUST** render shims as ordinary `Instance`s with a translucent `tint` (the
    `tint` field already exists on `Instance`), positioned by the predicted trajectory
    with `motionStart` in the future — same GPU motion formula as live sprites.
11. **MUST** add a second render pass for translucents: opaque pass first
    (depth write on, alpha cutout as today), then shim pass with blending enabled and
    **depth read-only** (test on, write off), sorted or order-tolerant.
    **Blocker (ground truth):** there is no separate translucent pass today; blended
    shims would z-fight. This is a renderer contract change — update ARCHITECTURE.md
    first per the doc-sync rule.
12. **MUST NOT** implement shims as HUD/UI overlay elements (hard constraint).

### Upgrades

13. **MUST** map Clare's upgrade tier to lookahead window and shim clarity per the
    tier table in [GAME_LAYER.md](../GAME_LAYER.md#clares-upgrades) — that table is
    the single source for values; do not duplicate numbers here.
14. **MUST** express clarity as render-side parameters (tint alpha, detail level such
    as outline-only vs full-color frames), so each tier visibly changes feel, not just
    numbers.
15. **SHOULD** store the current tier in persistent GameState (SCENE_COORDINATOR).

### Corruption effects (Amblico / Skivana)

16. **MUST** implement clairvoyance corruption as transforms applied to the lookahead
    **output only** — offsetting, hiding, or fabricating shims — never as changes to
    the real sim or the fork's rules. The world stays truthful; only Clare's *view* of
    the future lies.
17. **MUST** drive corruption from the seeded RNG / sim tick so corrupted shims are
    themselves deterministic and replayable.
18. **MAY** support per-boss corruption profiles in data (false-shim rate, positional
    noise amplitude, suppression windows).

### Determinism testing

19. **MUST** add a replay test: record an input sequence, run it twice (and
    cross-platform where feasible), assert bit-identical shim positions and trigger
    firing ticks. Extends the existing `CV_FIXED_TIME` / parity-test harness.
20. **SHOULD** run the replay test in `make test` so determinism regressions fail CI.

---

## Non-goals

- Long-range narrative visions (cutscene territory, not this system).
- Predicting other *players* — networked play is planned (see req 3b), but shims
  preview enemy attacks only.
- Full-fidelity AI in the fork — the lookahead only needs attack-phase accuracy.
- Shim interaction (shims are read-only previews; Clare dodges, she doesn't touch them).

---

## Open questions

- Player model inside the fork: frozen at current position, or extrapolated along
  current velocity? Affects how "honest" proximity-triggered attacks look in preview.
- Shim refresh cadence K (every tick vs every 5–10 ticks) — trade CPU vs responsiveness.
- Do shims preview projectiles already in flight (trivial — analytic motion) at tier 0,
  with AI-decision previews unlocking later tiers?

---

## Dependencies

- **Engine:** translucent second pass (req 11) — ARCHITECTURE.md change first.
- **SCENE_COORDINATOR:** shares the Scene/`SimState` separation (req 4) and the
  fixed-timestep epoch; upgrade tier lives in its GameState.
- **WORLD_BUILDING:** wall collision AABBs feed the fork (req 7).
- No dependency on MAP_EDITOR beyond scenes remaining immutable data.
