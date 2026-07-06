// test_text.cpp — bitmap font: makeText layout/frames, text entity scene
// loading (including the `remove`-despawns-the-whole-string behavior), and
// the dialogueText timed-expiry extension of the dialogue action.
#include "test_harness.h"

#include "text.h"
#include "scene.h"
#include "sim.h"
#include "events.h"

#include <cstdio>
#include <filesystem>
#include <string>

using namespace cv;
namespace fs = std::filesystem;

namespace {

bool loadFromString(const std::string& json, Scene& out, std::string& err) {
    fs::create_directories("build");
    const char* path = "build/test_text_tmp.json";
    std::FILE* f = std::fopen(path, "wb");
    if (!f) { err = "cannot open temp file"; return false; }
    std::fwrite(json.data(), 1, json.size(), f);
    std::fclose(f);
    bool ok = loadScene(path, out, err);
    fs::remove(path);
    return ok;
}

// makeText: layout along +X, per-glyph fields, and out-of-range byte handling.
void test_make_text() {
    std::vector<Instance> glyphs =
        makeText("Hi!", {1.0f, 2.0f, 3.0f}, 0.5f, 0.8f, 2, {0.1f, 0.2f, 0.3f, 0.4f});
    CHECK(glyphs.size() == 3);
    if (glyphs.size() != 3) return;

    CHECK(glyphs[0].pos.x == 1.0f);
    CHECK(glyphs[1].pos.x == 1.5f);
    CHECK(glyphs[2].pos.x == 2.0f);
    for (const Instance& g : glyphs) {
        CHECK(g.pos.y == 2.0f && g.pos.z == 3.0f);
        CHECK(g.scale.x == 0.5f && g.scale.y == 0.8f);
        CHECK(g.billboard == 0.0f);   // non-billboard quad, per spec
        CHECK(g.sheet == 2.0f);
        CHECK(g.tint.x == 0.1f && g.tint.w == 0.4f);
        CHECK(g.anim.y == 1.0f);      // frame count 1 (static glyph)
    }
    // Frame index = ascii - 32.
    CHECK(glyphs[0].anim.x == static_cast<float>('H' - 32));
    CHECK(glyphs[1].anim.x == static_cast<float>('i' - 32));
    CHECK(glyphs[2].anim.x == static_cast<float>('!' - 32));

    // Out-of-range byte → blank "space" cell (frame 0), not a wrong glyph.
    std::vector<Instance> highByte = makeText(std::string(1, static_cast<char>(200)),
                                              {0, 0, 0}, 1, 1, 0, {1, 1, 1, 1});
    CHECK(highByte.size() == 1 && highByte[0].anim.x == 0.0f);

    // Empty string → no glyphs.
    CHECK(makeText("", {0, 0, 0}, 1, 1, 0, {1, 1, 1, 1}).empty());
}

// Scene loader: a "text" entity expands to one glyph Instance per character;
// the entity name binds the first glyph; `remove` despawns the whole range.
void test_text_entity_load() {
    Scene scene;
    std::string err;
    bool ok = loadFromString(R"({
        "version": 1,
        "entities": [
            { "id": "title", "text": "HI", "pos": [1, 2, 3], "charSize": [0.4, 0.6],
              "sheet": 1, "tint": [1, 1, 1, 0.9] },
            { "id": "other", "pos": [0, 0, 0] }
        ]
    })", scene, err);
    CHECK_MSG(ok, err.c_str());
    if (!ok) return;

    EntityId first = scene.idOf("title");
    CHECK(first != 0);
    CHECK(scene.textRangeEnd.count(first) == 1);
    EntityId last = scene.textRangeEnd[first];
    CHECK(last == first + 1);   // "HI" → 2 glyphs

    const Instance& g0 = scene.initialState.instances[first];
    const Instance& g1 = scene.initialState.instances[last];
    CHECK(g0.pos.x == 1.0f && g0.pos.y == 2.0f && g0.pos.z == 3.0f);
    CHECK(g1.pos.x == 1.4f);   // pos.x + charW
    CHECK(g0.scale.x == 0.4f && g0.scale.y == 0.6f);
    CHECK(g0.sheet == 1.0f);
    CHECK(g0.tint.w == 0.9f);
    CHECK(g0.anim.x == static_cast<float>('H' - 32));
    CHECK(g1.anim.x == static_cast<float>('I' - 32));

    // A plain entity declared after a text entity gets an id right after the
    // text entity's glyph range (no gaps, no collisions).
    CHECK(scene.idOf("other") == last + 1);

    // Remove despawns every glyph of the text entity as one unit.
    Action remove;
    remove.type   = Action::Type::Remove;
    remove.entity = "title";
    Event e;
    e.trigger.type = Trigger::Type::Start;
    e.actions.push_back(remove);
    scene.events.push_back(e);

    SimState st = makeSimState(scene);
    Diff diff;
    stepSim(scene, st, ResolvedActions{}, diff);
    CHECK(st.entities.count(first) == 0);
    CHECK(st.entities.count(last) == 0);
    CHECK(st.entities.count(scene.idOf("other")) == 1);   // untouched
    CHECK(diff.removals.size() == 2);
}

// dialogueText config: the dialogue action also spawns text, which expires
// (and is removed via a Diff) after exactly DIALOGUE_TEXT_TICKS ticks.
void test_dialogue_text_expiry() {
    Scene scene;
    std::string err;
    bool ok = loadFromString(R"({
        "version": 1,
        "dialogueText": { "pos": [0, 1, 0], "charSize": [0.3, 0.3], "sheet": 0,
                          "tint": [1, 1, 1, 1] },
        "entities": [ { "id": "a", "pos": [0, 0, 0] } ],
        "events": [
            { "trigger": { "type": "start" },
              "actions": [ { "type": "dialogue", "id": "hi" } ] }
        ]
    })", scene, err);
    CHECK_MSG(ok, err.c_str());
    if (!ok) return;
    CHECK(scene.dialogueText.enabled);

    SimState st = makeSimState(scene);
    Diff diff0;
    stepSim(scene, st, ResolvedActions{}, diff0);   // start trigger fires dialogue

    auto countDynamic = [&]() {
        int n = 0;
        for (const auto& kv : st.entities)
            if (kv.first >= 1000000) ++n;
        return n;
    };
    CHECK(countDynamic() == 2);           // "hi" → 2 glyphs
    CHECK(diff0.upserts.size() == 2);
    CHECK(st.dialogueTextExpiry.size() == 1);

    // Spawn happened while state.clock.tick was still 0 (pre-increment), so
    // expiry = 0 + DIALOGUE_TEXT_TICKS; after the spawning stepSim call above,
    // the clock already reads 1. Advance to one tick before expiry.
    for (int i = 0; i < DIALOGUE_TEXT_TICKS - 2; ++i) {
        Diff d;
        stepSim(scene, st, ResolvedActions{}, d);
    }
    CHECK(st.clock.tick == static_cast<uint64_t>(DIALOGUE_TEXT_TICKS - 1));
    CHECK(countDynamic() == 2);           // not expired yet

    Diff dExpire;
    stepSim(scene, st, ResolvedActions{}, dExpire);   // crosses the expiry tick
    CHECK(countDynamic() == 0);
    CHECK(dExpire.removals.size() == 2);
    CHECK(st.dialogueTextExpiry.empty());
}

} // namespace

void test_text() {
    test_make_text();
    test_text_entity_load();
    test_dialogue_text_expiry();
}
