// test_dialogue.cpp — conversations, the inventory, pickups and the overlay.
#include "test_harness.h"

#include "dialogue.h"
#include "events.h"
#include "scene.h"
#include "sim.h"
#include "text.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <set>
#include <string>

using namespace cv;
namespace fs = std::filesystem;

namespace {

bool loadFromString(const std::string& json, Scene& out, std::string& err) {
    fs::create_directories("build");
    const char* path = "build/test_dialogue_tmp.json";
    std::FILE* f = std::fopen(path, "wb");
    if (!f) { err = "cannot open temp file"; return false; }
    std::fwrite(json.data(), 1, json.size(), f);
    std::fclose(f);
    bool ok = loadScene(path, out, err);
    fs::remove(path);
    return ok;
}

// A hero, a baker one step away, a far-off hermit, and a coin to pick up.
const char* SHOP = R"({
  "version": 1,
  "sheets": [ {"path": "art/penguin.png", "cols": 16, "rows": 1},
              {"path": "art/font.png", "cols": 16, "rows": 6},
              {"path": "art/ui.png", "cols": 8, "rows": 2} ],
  "ui": {"sheet": 2, "font": 1},
  "labelStyle": {"sheet": 1},
  "items": {"coin": "Coin", "bun": {"name": "Sweet bun"}},
  "controls": {"bindings": {"space": "interact", "enter": "interact", "w": "move_north"}},
  "archetypes": {
    "hero":   {"name": "Clare", "controlled": true, "speed": 3.0, "scale": [0.9, 0.9],
               "offset": [0, 0.32],
               "clips": {"idle": {"first": 7}, "chat": {"first": 4, "count": 2, "fps": 5}}},
    "baker":  {"name": "Baker Bo", "label": "NPC", "talk": "baker", "tint": [1, 0.8, 0.6, 1],
               "anim": "idle",
               "clips": {"idle": {"first": 7}, "chat": {"first": 4, "count": 2, "fps": 5}}},
    "hermit": {"talk": "hermit"},
    "coin":   {"pickup": "coin", "label": "OBJECT"}
  },
  "entities": [
    {"id": "hero",   "archetype": "hero",   "pos": [0, 0, 0]},
    {"id": "baker",  "archetype": "baker",  "pos": [1, 0, 0]},
    {"id": "hermit", "archetype": "hermit", "pos": [40, 0, 0]},
    {"id": "coin1",  "archetype": "coin",   "pos": [0, 0, 5]},
    {"id": "purse",  "archetype": "coin",   "pos": [0, 0, 9], "pickup": {"item": "coin", "count": 3}}
  ],
  "dialogues": {
    "baker": [
      {"if": {"flag": "fed"}, "speaker": "baker", "text": "Enjoy the bun!", "end": true},
      {"if": {"flag": "met", "value": 0}, "speaker": "baker", "text": "Welcome to my bakery.",
       "set": {"met": 1}},
      {"speaker": "baker", "text": "A sweet bun costs two coins. Hungry?",
       "choices": [ {"text": "Yes please.", "set": {"want": 1}},
                    {"text": "No thanks.",  "set": {"want": 0}} ]},
      {"if": {"all": [{"flag": "want"}, {"item": "coin", "op": "ge", "value": 2}]},
       "speaker": "baker", "text": "Here you go, fresh from the oven.",
       "take": {"coin": 2}, "give": {"bun": 1}, "set": {"fed": 1, "want": 0}, "end": true},
      {"if": {"flag": "want"}, "speaker": "hero", "text": "I can't afford it.", "set": {"want": 0}},
      {"if": {"lacks": "bun"}, "text": "The smell of bread follows you out."}
    ],
    "hermit": [ {"if": {"has": "bun"}, "text": "Is that a bun?"} ]
  }
})";

Vec3 posOf(const SimState& st, EntityId id) {
    return motionAt(st.entities.at(id), static_cast<float>(st.clock.time()));
}

// One tick, with keys. Collects what the renderer was told into `screen`:
// the set of overlay instance ids currently on screen.
struct Screen {
    std::set<EntityId> overlay;
    void apply(const Diff& d) {
        for (const auto& up : d.upserts)
            if (up.second.layer == LAYER_OVERLAY) overlay.insert(up.first);
        for (EntityId id : d.removals) overlay.erase(id);
    }
};

void tick(const Scene& scene, SimState& st, Screen& screen,
          const char* pressed = nullptr, const char* held = nullptr) {
    ResolvedActions a;
    if (pressed) { a.pressed.insert(pressed); a.down.insert(pressed); }
    if (held) a.down.insert(held);
    Diff d;
    stepSim(scene, st, a, d);
    screen.apply(d);
}

void wait(const Scene& scene, SimState& st, Screen& screen, int ticks) {
    for (int i = 0; i < ticks; ++i) tick(scene, st, screen);
}

const std::string& lineText(const Scene& scene, const SimState& st) {
    static const std::string none;
    if (!st.talk.active) return none;
    return scene.dialogues.at(st.talk.id).lines[st.talk.line].text;
}

// Press interact until the line on screen is done typing and gives way to the
// next one (or the box closes). With `choice`, moves the cursor there first.
void advance(const Scene& scene, SimState& st, Screen& screen, int choice = -1) {
    if (!st.talk.revealed || !st.talk.footBuilt) tick(scene, st, screen, "interact");  // finish typing
    tick(scene, st, screen);
    for (int i = 0; choice >= 0 && i < choice; ++i) tick(scene, st, screen, "move_south");
    tick(scene, st, screen, "interact");
}

void teleport(SimState& st, EntityId id, float x, float z) {
    Instance& inst = st.entities.at(id);
    inst.pos = {x, inst.pos.y, z};
    setMotion(inst, {0, 0, 0}, {0, 0, 0}, static_cast<float>(st.clock.time()));
}

size_t groupTotal(const SimState& st) {
    size_t n = 0;
    for (const auto& kv : st.ui.groups) n += kv.second.size();
    return n;
}

// --- text layout ---------------------------------------------------------------

void test_text_block() {
    // Proportional: an "i" is narrower than an "m"; a string is the sum of its letters.
    CHECK(textWidth("i", 32) < textWidth("m", 32));
    CHECK(std::fabs(textWidth("im", 32) - textWidth("i", 32) - textWidth("m", 32)) < 1e-4f);
    CHECK(textWidth("", 32) == 0.0f);

    TextBlock one = makeTextBlock("Hello there", 100, 500, 32, 4096, 36, 3, 1, {1, 1, 1, 1});
    CHECK(one.lines == 1);
    CHECK(one.glyphs.size() == 10);                 // the space draws nothing
    CHECK(std::fabs(one.width - textWidth("Hello there", 32)) < 1e-3f);
    for (const Instance& g : one.glyphs) {
        CHECK(g.layer == LAYER_OVERLAY);
        CHECK(g.pos.z == 3.0f && g.sheet == 1.0f);
        CHECK(std::fabs(g.pos.y - (500 - 16)) < 1e-3f);
    }
    for (size_t i = 1; i < one.glyphs.size(); ++i)
        CHECK(one.glyphs[i].pos.x > one.glyphs[i - 1].pos.x);   // reading order
    CHECK(one.glyphs[0].anim.x == static_cast<float>('H' - 32));

    // Wrapping breaks between words, never inside one, and never indents.
    const float narrow = textWidth("Hello ", 32) + 4;
    TextBlock two = makeTextBlock("Hello there", 100, 500, 32, narrow, 36, 0, 1, {1, 1, 1, 1});
    CHECK(two.lines == 2);
    CHECK(two.glyphs.size() == 10);
    CHECK(std::fabs(two.glyphs[5].pos.y - (500 - 16 - 36)) < 1e-3f);      // 't' of "there"
    CHECK(two.glyphs[5].pos.x < two.glyphs[1].pos.x);                       // back at the margin
    CHECK(two.width <= narrow);

    CHECK(makeTextBlock("a\nb", 0, 0, 32, 4096, 36, 0, 1, {1, 1, 1, 1}).lines == 2);
    CHECK(makeTextBlock("", 0, 0, 32, 4096, 36, 0, 1, {1, 1, 1, 1}).lines == 0);
}

// --- loading -------------------------------------------------------------------

void test_dialogue_load() {
    Scene scene;
    std::string err;
    CHECK_MSG(loadFromString(SHOP, scene, err), err.c_str());
    CHECK(scene.ui.enabled && scene.ui.sheet == 2 && scene.ui.fontSheet == 1);
    CHECK(scene.dialogues.size() == 2);
    const Dialogue& baker = scene.dialogues.at("baker");
    CHECK(baker.lines.size() == 6);
    CHECK(baker.lines[2].choices.size() == 2);
    CHECK(baker.lines[2].choices[0].set.size() == 1);
    CHECK(baker.lines[3].condition.kind == Condition::Kind::All);
    CHECK(baker.lines[3].condition.children[1].kind == Condition::Kind::Item);
    CHECK(baker.lines[3].take.size() == 1 && baker.lines[3].give.size() == 1 && baker.lines[3].end);
    CHECK(baker.lines[5].speaker.empty());          // narration
    CHECK(baker.lines[5].condition.kind == Condition::Kind::Item);
    CHECK(baker.lines[5].condition.op == Condition::Op::Lt);

    // Speakers come from archetypes: name, tint, and the chat clip as portrait.
    const Speaker& who = scene.speakers.at("baker");
    CHECK(who.name == "Baker Bo");
    CHECK(who.first == 4 && who.count == 2 && who.fps == 5.0f);
    CHECK(who.tint.y == 0.8f);
    CHECK(scene.speakers.at("hermit").name == "hermit");   // falls back to the id

    CHECK(scene.itemName("bun") == "Sweet bun");
    CHECK(scene.itemName("coin") == "Coin");
    CHECK(scene.itemName("pebble") == "pebble");

    CHECK(scene.talkIds.size() == 2 && scene.pickupIds.size() == 2);
    CHECK(scene.interactables.at(scene.idOf("baker")).name == "Baker Bo");
    CHECK(scene.interactables.at(scene.idOf("purse")).count == 3);

    // Errors name the place.
    Scene bad;
    CHECK(!loadFromString(R"({"version": 1, "ui": {}, "archetypes": {"a": {}},
        "dialogues": {"d": [ {"speaker": "nobody", "text": "hi"} ]} })", bad, err));
    CHECK(err.find("dialogues.d[0].speaker") != std::string::npos);
    Scene bad2;
    CHECK(!loadFromString(R"({"version": 1, "ui": {},
        "dialogues": {"d": [ {"text": "ok"}, {"speaker": ""} ]} })", bad2, err));
    CHECK(err.find("dialogues.d[1]") != std::string::npos);
    Scene bad3;
    CHECK(!loadFromString(R"({"version": 1, "dialogues": {"d": [ {"text": "ok"} ]} })", bad3, err));
    CHECK(err.find("'ui'") != std::string::npos);
    Scene bad4;
    CHECK(!loadFromString(R"({"version": 1, "ui": {},
        "entities": [ {"id": "x", "talk": "missing"} ] })", bad4, err));
    CHECK(err.find("unknown dialogue 'missing'") != std::string::npos);
    Scene bad5;
    CHECK(!loadFromString(R"({"version": 1, "ui": {}, "dialogues": {"d": [
        {"text": "pick", "choices": [ {"set": {"a": 1}} ]} ]} })", bad5, err));
    CHECK(err.find("choices[0]") != std::string::npos);
    Scene bad6;
    CHECK(!loadFromString(R"({"version": 1, "events": [
        {"actions": [ {"type": "talk", "id": "nope"} ]} ] })", bad6, err));
    CHECK(err.find("unknown dialogue 'nope'") != std::string::npos);
}

// --- a conversation, start to finish -------------------------------------------

void test_dialogue_flow() {
    Scene scene;
    std::string err;
    CHECK_MSG(loadFromString(SHOP, scene, err), err.c_str());
    EntityId hero = scene.idOf("hero"), baker = scene.idOf("baker");
    SimState st = makeSimState(scene);
    Screen screen;

    // Standing next to the baker: a hint says who interact would talk to.
    tick(scene, st, screen);
    CHECK(st.ui.hintTarget == baker);
    CHECK(!screen.overlay.empty());
    CHECK(!st.talk.active);

    // Interact opens the conversation on the first line that passes its filter.
    tick(scene, st, screen, "interact");
    CHECK(st.talk.active && st.talk.id == "baker" && st.talk.partner == baker);
    CHECK(lineText(scene, st) == "Welcome to my bakery.");
    CHECK(st.flags["met"] == 1.0);                 // a line's "set" applies as it comes up
    CHECK(st.ui.hintTarget == 0);                  // the hint steps aside
    CHECK(st.entities.at(baker).anim.x == 4.0f);   // the baker starts chatting

    // The text is typed out a few glyphs at a time...
    CHECK(!st.talk.revealed);
    const int early = st.talk.shown;
    wait(scene, st, screen, 6);
    CHECK(st.talk.shown > early && !st.talk.revealed);
    // ...and interact finishes the line instead of skipping it.
    tick(scene, st, screen, "interact");
    CHECK(st.talk.revealed && lineText(scene, st) == "Welcome to my bakery.");
    CHECK(st.talk.shown == 18);                    // every letter of the line

    // While talking, the hero does not move and the keys do not reach the world.
    Vec3 before = posOf(st, hero);
    for (int i = 0; i < 30; ++i) tick(scene, st, screen, nullptr, "move_north");
    CHECK(posOf(st, hero).z == before.z);

    // Next line: a question. Left alone, it types itself out and offers choices.
    tick(scene, st, screen, "interact");
    CHECK(lineText(scene, st) == "A sweet bun costs two coins. Hungry?");
    wait(scene, st, screen, 120);
    CHECK(st.talk.revealed && st.talk.footBuilt && st.talk.choice == 0);
    // Up/down move the cursor and wrap around.
    tick(scene, st, screen, "move_south");
    CHECK(st.talk.choice == 1);
    tick(scene, st, screen, "move_south");
    CHECK(st.talk.choice == 0);
    tick(scene, st, screen, "move_north");
    CHECK(st.talk.choice == 1);
    tick(scene, st, screen, "move_north");
    CHECK(st.talk.choice == 0);

    // "Yes please" sets a variable; with no coins, the line that needs two is
    // skipped and the conversation takes the other branch.
    tick(scene, st, screen, "interact");
    CHECK(st.flags["want"] == 0.0);                // ...already cleared by the line it led to
    CHECK(lineText(scene, st) == "I can't afford it.");
    CHECK(st.inventory.empty());
    advance(scene, st, screen);
    CHECK(lineText(scene, st) == "The smell of bread follows you out.");
    // The last line: one more interact and the box closes.
    advance(scene, st, screen);
    CHECK(!st.talk.active);
    CHECK(st.entities.at(baker).anim.x == 7.0f);   // back to idle
    tick(scene, st, screen);
    CHECK(st.ui.hintTarget == baker);              // and the hint returns

    // Second visit: "met" is set, so the greeting is filtered out.
    tick(scene, st, screen, "interact");
    CHECK(lineText(scene, st) == "A sweet bun costs two coins. Hungry?");
    // "No thanks": nothing wanted, straight to the closing narration.
    advance(scene, st, screen, 1);
    CHECK(lineText(scene, st) == "The smell of bread follows you out.");
    advance(scene, st, screen);
    CHECK(!st.talk.active);

    // With coins in the pocket the same list plays the buying branch.
    Diff d;
    giveItem(scene, st, "coin", 3, d);
    screen.apply(d);
    tick(scene, st, screen, "interact");
    advance(scene, st, screen, 0);
    CHECK(lineText(scene, st) == "Here you go, fresh from the oven.");
    CHECK(st.inventory.at("coin") == 1 && st.inventory.at("bun") == 1);
    CHECK(st.flags["fed"] == 1.0);
    advance(scene, st, screen);                    // "end": true closes right here
    CHECK(!st.talk.active);

    // From now on the baker has one thing to say.
    tick(scene, st, screen, "interact");
    CHECK(lineText(scene, st) == "Enjoy the bun!");
    advance(scene, st, screen);
    CHECK(!st.talk.active);

    // Nothing is left on screen but what the sim still tracks (hint, inventory, toast).
    CHECK(screen.overlay.size() == groupTotal(st));
    CHECK(st.ui.groups.count("dialogue.box") == 0 && st.ui.groups.count("dialogue.text") == 0);

    // A dialogue whose every line is filtered out never opens.
    SimState fresh = makeSimState(scene);
    Diff d2;
    startDialogue(scene, fresh, "hermit", 0, d2);
    CHECK(!fresh.talk.active && d2.upserts.empty());
    startDialogue(scene, fresh, "no such dialogue", 0, d2);
    CHECK(!fresh.talk.active);
}

// --- pickups, inventory, HUD ----------------------------------------------------

void test_pickups() {
    Scene scene;
    std::string err;
    CHECK_MSG(loadFromString(SHOP, scene, err), err.c_str());
    EntityId hero = scene.idOf("hero"), coin = scene.idOf("coin1"), purse = scene.idOf("purse");
    SimState st = makeSimState(scene);
    Screen screen;
    tick(scene, st, screen);
    CHECK(st.inventory.empty());
    CHECK(st.ui.groups.count("hud.inventory") == 0);

    // Walk onto the coin: it is gone from the world, label and all, and counted.
    const std::vector<EntityId> label = scene.labels.at(coin);
    CHECK(!label.empty());
    teleport(st, hero, 0.0f, 4.7f);
    tick(scene, st, screen);
    CHECK(st.inventory.at("coin") == 1);
    CHECK(st.entities.count(coin) == 0 && st.entities.count(label[0]) == 0);
    tick(scene, st, screen);
    CHECK(st.inventory.at("coin") == 1);           // not collected twice
    CHECK(st.ui.groups.count("hud.inventory") == 1);
    CHECK(st.ui.groups.count("hud.toast") == 1);   // "+1 Coin"

    // A pickup can be worth several.
    teleport(st, hero, 0.3f, 9.2f);
    tick(scene, st, screen);
    CHECK(st.inventory.at("coin") == 4);
    CHECK(st.entities.count(purse) == 0);

    // The toast goes away on its own; the inventory stays.
    wait(scene, st, screen, TOAST_TICKS + 2);
    CHECK(st.ui.groups.count("hud.toast") == 0);
    CHECK(st.ui.groups.count("hud.inventory") == 1);
    CHECK(screen.overlay.size() == groupTotal(st));

    // Out of everyone's reach: no hint. Back in reach: it comes back.
    CHECK(st.ui.hintTarget == 0);
    teleport(st, hero, 0.0f, TALK_REACH - 0.1f);   // ~1.67 from the baker at (1, 0): too far
    tick(scene, st, screen);
    CHECK(st.ui.hintTarget == 0);
    teleport(st, hero, 0.0f, 0.5f);
    tick(scene, st, screen);
    CHECK(st.ui.hintTarget == scene.idOf("baker"));

    // Taking everything of an item removes its row; conditions read counts.
    Condition has;
    has.kind = Condition::Kind::Item; has.flag = "coin"; has.op = Condition::Op::Ge; has.value = 4;
    CHECK(conditionPasses(st, has));
    Diff d;
    giveItem(scene, st, "coin", -10, d);
    CHECK(st.inventory.count("coin") == 0);
    CHECK(!conditionPasses(st, has));
}

// --- events can talk, toast and give ---------------------------------------------

void test_event_actions() {
    Scene scene;
    std::string err;
    CHECK_MSG(loadFromString(R"({
      "version": 1,
      "sheets": [ {"path": "art/penguin.png", "cols": 16, "rows": 1},
                  {"path": "art/font.png", "cols": 16, "rows": 6},
                  {"path": "art/ui.png", "cols": 8, "rows": 2} ],
      "ui": {"sheet": 2, "font": 1},
      "entities": [ {"id": "hero", "pos": [0, 0, 0], "controlled": true},
                    {"id": "door", "pos": [5, 0, 0], "solid": true} ],
      "dialogues": { "intro": [ {"text": "Once upon a time."} ] },
      "events": [
        {"trigger": {"type": "start"}, "actions": [ {"type": "talk", "id": "intro"},
                                                    {"type": "give", "item": "key"} ]},
        {"trigger": {"type": "always"}, "condition": {"flag": "open"}, "once": true,
         "actions": [ {"type": "remove", "entity": "door"}, {"type": "toast", "text": "Open!"} ]}
      ]
    })", scene, err), err.c_str());
    SimState st = makeSimState(scene);
    Screen screen;
    tick(scene, st, screen);
    CHECK(st.talk.active && lineText(scene, st) == "Once upon a time.");
    CHECK(st.inventory.at("key") == 1);
    advance(scene, st, screen);
    CHECK(!st.talk.active);

    // "always" waits for its condition, then fires once.
    wait(scene, st, screen, 10);
    CHECK(st.entities.count(scene.idOf("door")) == 1);
    st.flags["open"] = 1.0;
    tick(scene, st, screen);
    CHECK(st.entities.count(scene.idOf("door")) == 0);
    CHECK(st.ui.groups.count("hud.toast") == 1);
}

// --- the village demo, played through ---------------------------------------------

// Can the player walk from one cell to another? Flood fill over cell centres.
bool reachable(const Scene& scene, const SimState& st, int fromX, int fromZ, int toX, int toZ) {
    const float r = 0.3f;
    auto open = [&](int x, int z) {
        if (scene.terrain.blocksCircle(x + 0.5f, z + 0.5f, r)) return false;
        for (EntityId id : scene.solidIds) {
            auto it = st.entities.find(id);
            if (it == st.entities.end()) continue;
            float dx = x + 0.5f - it->second.pos.x, dz = z + 0.5f - it->second.pos.z;
            float reach = r + scene.attrs.at(id).radius;
            if (dx * dx + dz * dz < reach * reach) return false;
        }
        return true;
    };
    std::set<std::pair<int, int>> seen;
    std::deque<std::pair<int, int>> todo;
    todo.push_back({fromX, fromZ});
    seen.insert({fromX, fromZ});
    while (!todo.empty()) {
        auto [x, z] = todo.front();
        todo.pop_front();
        if (std::abs(x - toX) + std::abs(z - toZ) <= 1) return true;   // next to it is enough
        const int dirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
        for (const auto& d : dirs) {
            int nx = x + d[0], nz = z + d[1];
            if (nx < -8 || nz < -8 || nx > 48 || nz > 40) continue;
            if (seen.count({nx, nz}) || !open(nx, nz)) continue;
            seen.insert({nx, nz});
            todo.push_back({nx, nz});
        }
    }
    return false;
}

void test_village_quest() {
    Scene scene;
    std::string err;
    bool ok = loadScene("src/levels/village.json", scene, err);
    CHECK_MSG(ok, err.c_str());
    if (!ok) return;

    EntityId hero = scene.rigFollow;
    SimState st = makeSimState(scene);
    Screen screen;
    auto cellOf = [&](EntityId id, int& x, int& z) {
        Vec3 p = posOf(st, id);
        x = static_cast<int>(std::floor(p.x));
        z = static_cast<int>(std::floor(p.z));
    };
    auto standBy = [&](const char* name) {
        Vec3 p = posOf(st, scene.idOf(name));
        teleport(st, hero, p.x, p.z + 1.0f);
        tick(scene, st, screen);
    };
    auto readToEnd = [&](int choice = -1) {
        for (int guard = 0; guard < 40 && st.talk.active; ++guard) {
            const DialogueLine& line = scene.dialogues.at(st.talk.id).lines[st.talk.line];
            advance(scene, st, screen, line.choices.empty() ? -1 : choice);
        }
        CHECK(!st.talk.active);
    };
    auto talkTo = [&](const char* name, int choice = -1) {
        standBy(name);
        tick(scene, st, screen, "interact");
        CHECK(st.talk.active);
        std::string first = lineText(scene, st);
        readToEnd(choice);
        return first;
    };

    // The scene opens on its introduction.
    tick(scene, st, screen);
    CHECK(st.talk.active && st.talk.id == "intro");
    readToEnd();

    // Everything the quest needs can be walked to from the start — except
    // what lies behind the gate.
    int hx, hz;
    cellOf(hero, hx, hz);
    int herbs = 0, coins = 0;
    EntityId chest = 0;
    for (EntityId id : scene.pickupIds) {
        const Interactable& what = scene.interactables.at(id);
        int x, z;
        cellOf(id, x, z);
        if (what.item == "crown") { chest = id; CHECK(!reachable(scene, st, hx, hz, x, z)); continue; }
        CHECK(reachable(scene, st, hx, hz, x, z));
        if (what.item == "herb") herbs += what.count;
        if (what.item == "coin") coins += what.count;
    }
    CHECK(herbs >= 3 && coins >= 2 && chest != 0);
    for (EntityId id : scene.talkIds) {
        int x, z;
        cellOf(id, x, z);
        CHECK(reachable(scene, st, hx, hz, x, z));
    }

    // The guard turns you away; the sleeper sleeps.
    CHECK(talkTo("guard@19,11", 2) == "Halt. The old ruins are sealed, by order of the elder.");
    CHECK(st.flags["gate_open"] == 0.0);
    CHECK(talkTo("sleeper@13,20") == "Zzz... five more winters...");

    // The elder asks for herbs; say yes (and ask what's in it).
    CHECK(talkTo("elder@15,16", 1) == "Oh! A traveller. I am Pim, elder of this village, such as it is.");
    CHECK(st.flags["herb_quest"] == 1.0 && st.flags["greedy"] == 1.0 && st.flags["met_elder"] == 1.0);
    // Come back empty-handed: a reminder, not a second introduction.
    CHECK(talkTo("elder@15,16").find("Still looking?") == 0);

    // Gather everything lying around.
    for (EntityId id : scene.pickupIds) {
        if (id == chest || !st.entities.count(id)) continue;
        Vec3 p = posOf(st, id);
        teleport(st, hero, p.x, p.z);
        tick(scene, st, screen);
        CHECK(st.entities.count(id) == 0);
    }
    CHECK(st.inventory.at("herb") == herbs && st.inventory.at("coin") == coins);

    // Herbs for the key — the greedy answer gets the lecture.
    CHECK(talkTo("elder@15,16") == "Three moonherbs! My knees thank you already.");
    CHECK(st.inventory.at("key") == 1 && st.flags["herb_quest"] == 2.0);
    CHECK(st.inventory.count("herb") == (herbs > 3 ? 1u : 0u));
    CHECK(st.flags["elder_paying"] == 0.0);

    // A key but no lantern: still no entry.
    talkTo("guard@19,11", 2);
    CHECK(st.flags["gate_open"] == 0.0);

    // Coins for the lantern.
    talkTo("merchant@22,15", 0);
    CHECK(st.inventory.at("lantern") == 1 && st.flags["bought_lantern"] == 1.0);
    CHECK(st.inventory.count("coin") == (coins > 2 ? 1u : 0u));
    CHECK(talkTo("merchant@22,15") == "Mind the lantern oil. No refunds!");

    // The lantern wakes the sleeper.
    CHECK(talkTo("sleeper@13,20").find("Agh!") == 0);

    // Key and lantern: the guard opens the gate, and the way in is clear.
    EntityId gate = scene.idOf("gate@18,9");
    CHECK(st.entities.count(gate) == 1);
    talkTo("guard@19,11");
    CHECK(st.flags["gate_open"] == 1.0);
    tick(scene, st, screen);
    CHECK(st.entities.count(gate) == 0);
    int cx, cz;
    cellOf(chest, cx, cz);
    CHECK(reachable(scene, st, hx, hz, cx, cz));

    // The chest: the crown, and a line of narration about it.
    Vec3 c = posOf(st, chest);
    teleport(st, hero, c.x, c.z);
    tick(scene, st, screen);
    CHECK(st.inventory.at("crown") == 1);
    tick(scene, st, screen);
    CHECK(st.talk.active && st.talk.id == "found_crown");
    readToEnd();

    // The elder has the last word.
    CHECK(talkTo("elder@15,16") == "The lost crown! So the old stories were true...");

    // The screen holds exactly what the sim tracks — nothing leaked.
    CHECK(screen.overlay.size() == groupTotal(st));

    // Determinism: the same key presses, replayed, give the same game.
    auto play = [&](SimState& s) {
        Screen scr;
        for (int t = 0; t < 1500; ++t) {
            ResolvedActions a;
            if (t % 45 == 0) { a.pressed.insert("interact"); a.down.insert("interact"); }
            if (t % 200 == 100) a.pressed.insert("move_south");
            int phase = (t / 120) % 4;
            a.down.insert(phase == 0 ? "move_north" : phase == 1 ? "move_west"
                        : phase == 2 ? "move_north" : "move_east");
            Diff d;
            stepSim(scene, s, a, d);
        }
    };
    SimState a = makeSimState(scene), b = makeSimState(scene);
    play(a);
    play(b);
    CHECK(a.flags == b.flags && a.inventory == b.inventory);
    CHECK(a.talk.active == b.talk.active && a.talk.line == b.talk.line);
    CHECK(a.nextDynamicId == b.nextDynamicId);
    CHECK(posOf(a, hero).x == posOf(b, hero).x && posOf(a, hero).z == posOf(b, hero).z);
    CHECK(a.flags.count("met_elder") == 1);        // the walk really did reach the elder
}

} // namespace

void test_dialogue() {
    test_text_block();
    test_dialogue_load();
    test_dialogue_flow();
    test_pickups();
    test_event_actions();
    test_village_quest();
}
