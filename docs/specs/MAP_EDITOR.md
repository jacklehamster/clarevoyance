# Spec: Map Editor & Scene Format Tooling

Requirements for tooling that reads and writes scene JSON, and for the format
hardening that makes such tooling safe.

The scene schema today is documented in
[GAME_LAYER.md](../GAME_LAYER.md#script-layer-scene-files); engine contracts in
[ARCHITECTURE.md](../ARCHITECTURE.md).

---

## Purpose

Scene files are currently hand-written and the loader is silently permissive — typos
become defaults, not errors, and there is no writer, so no tool can round-trip a scene.
Before an editor exists, the format itself must become strict, versioned, and
serializable. The editor then places semantic things (archetypes, named clips), not
raw frame numbers.

---

## Requirements

### Strict loader (shared engine/editor)

1. **[PARTLY IMPLEMENTED]** **MUST** provide a strict-mode loader that errors (with
   context) on: unknown keys, wrong value types, duplicate entity/spawn ids, and
   unresolved entity references in events/actions.
   *Done:* duplicate entity ids, unresolved entity/archetype/clip references,
   malformed vectors, unknown trigger/action/op/edge strings — all with contextual
   messages (`events[3].actions[0]: unknown action type 'set_flg'`).
   *Remaining:* unknown-key detection and exhaustive value-type checks.
2. **[IMPLEMENTED]** **MUST** share one loader implementation between the engine and
   the editor — `loadScene` (src/game/scene.cpp) is the only loader and strictness is
   its only mode (no flag).
3. **[IMPLEMENTED]** **MUST** fix the current behavior where unknown trigger/action
   types silently fall back to defaults — unknown types are errors.

### Schema as the single format spec

4. **[IMPLEMENTED]** **MUST** check a JSON Schema file into the repo — see
   [`scene.schema.json`](scene.schema.json) (draft-07, format v1): the single
   normative definition of the scene format. Prose docs reference it; they do not
   redefine it.
5. **[IMPLEMENTED]** **MUST** add a top-level `"version"` integer field to scene
   JSON. The loader warns (stderr) when it is missing and rejects versions newer
   than it supports (currently 1); format changes bump the version and note the
   migration.
6. **SHOULD** validate all checked-in scenes against the schema in CI / `make test`.
   (`make test-unit` loads every scene through the strict loader; JSON-Schema
   validation in CI is still open.)

### Serializer (round-trip)

7. **[IMPLEMENTED]** **MUST** implement a JSON writer — `cv::serialize(JsonValue,
   indent)` in src/game/json.h emits any parsed document.
8. **[IMPLEMENTED]** **MUST** round-trip losslessly: objects are insertion-ordered,
   so parse → serialize preserves key order and array order (verified byte-for-byte
   in tests/test_serialize.cpp); numbers use the shortest representation that parses
   back to the identical double.
9. **MUST NOT** serialize runtime state (e.g. event `fired`) into scene files — scenes
   are immutable definitions (see SCENE_COORDINATOR / SHIM_SYSTEM state split).
   (Holds by construction: runtime state lives in `SimState`, never in scene JSON.)

### Archetypes and named animation clips

10. **[IMPLEMENTED]** **MUST** support entity archetypes/prefabs: a named template
    (sheet, scale, billboard, speed, default clip) that entity instances reference
    and override, e.g. `{ "id": "guard_1", "archetype": "road_guard", "pos": [...] }`.
    Declared in the scene's top-level `"archetypes"` object; sharing archetype files
    across scenes is still open.
11. **[IMPLEMENTED]** **MUST** support named animation clips (`"walk"`, `"idle"`,
    `"windup"`) defined once per archetype, mapping to `first/count/fps`. `set_anim`
    (via `"clip"`) and entity definitions (string `"anim"`) accept clip names; raw
    frame indices remain valid but are discouraged in authored content. The editor
    places clips, never frame numbers.

### Asset paths and canonical directory

12. **MUST** resolve all asset paths (`sheet.path`, scene references) relative to one
    defined content root, identically on desktop and WASM.
13. **MUST** establish `src/levels/` as the single canonical scene directory. The
    WASM build path-maps or preloads it directly (Emscripten `--preload-file` supports
    `src@dst` mapping) — the duplicated `scenes/` copy is removed. `scenes/demo.json`
    is a divergent legacy format today and **MUST** be deleted or migrated, not kept
    in sync by hand.

### Editor

14. **MUST** let a designer: place/move/delete entities and tiles, assign archetypes
    and clips, set spawn points, and author events (trigger/condition/action forms
    driven by the schema — no free-text JSON required for common cases).
15. **MUST** write files only through the shared serializer and validate through the
    shared strict loader before saving.
16. **SHOULD** offer a live preview using the real renderer or a faithful projection
    of it (correct camera, billboard sizing).
17. **MAY** start as a validating CLI (`scene lint`, `scene fmt`) before any GUI —
    requirements 1–13 deliver most of the value without UI.

---

## Non-goals

- Editing sprite sheets (that is Aseprite's job).
- A general scripting language — the event system stays declarative data (determinism
  requirement, GAME_LAYER.md).
- Editing engine-side config (shaders, build flags).
- Runtime hot-reload of scenes (nice later; SCENE_COORDINATOR swap covers the need).

---

## Open questions

- ~~**Delivery form:**~~ **Decided (owner): in-engine edit mode.** The editor is a mode
  of the engine itself — it reuses the real renderer, camera, and strict loader
  directly, so preview fidelity and loader sharing are solved by construction.
- ~~Where does the strict loader live?~~ Resolved by the in-engine decision: the C++
  loader is the only loader; the editor calls it in-process.
- Schema evolution policy: strict rejection of newer minor versions, or
  forward-compatible ignore-with-warning?

---

## Dependencies

- **WORLD_BUILDING:** the `tiles` section and chunk format must be in the schema
  before the editor can place geometry.
- **SCENE_COORDINATOR:** `spawns` and `change_scene` are schema additions gated on the
  `version` field introduced here.
- **Engine:** none beyond existing loader code — this spec is mostly game-layer and
  tooling; any `Diff`/resource vocabulary change is owned by WORLD_BUILDING.
