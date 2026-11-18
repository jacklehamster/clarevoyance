// dialogue.cpp — see dialogue.h.
#include "dialogue.h"
#include "events.h"
#include "text.h"

#include <cctype>
#include <cmath>

namespace cv {

namespace {

// --- Screen layout (overlay units: 1280 x 720, origin bottom-left) -------------

// The window frame is drawn from nine cells of the UI sheet; corners are this
// big on screen.
const float FRAME_CORNER = 24.0f;

// UI sheet cells (scripts/gen_ui.py).
const int UI_FRAME_FIRST = 0;    // 0..8: frame, row-major from the top-left corner
const int UI_CURSOR      = 10;   // triangle pointing right
const int UI_MORE        = 11;   // triangle pointing down; 12 is empty (blink pair)
const float MORE_BLINK_FPS = 2.5f;

// The dialogue window, along the bottom of the screen.
const float BOX_X0 = 40.0f,  BOX_X1 = 1240.0f;
const float BOX_Y0 = 20.0f,  BOX_Y1 = 226.0f;
const float BOX_PAD = 22.0f;

// The speaker's badge, inside the window on the left.
const float BADGE_SIZE     = 166.0f;
const float BADGE_X0       = BOX_X0 + 18.0f;
const float BADGE_Y0       = BOX_Y0 + 20.0f;
const float PORTRAIT_SIZE  = 172.0f;   // the sprite cell has margins; let it fill the badge
const float PORTRAIT_DROP  = 10.0f;    // ...and sit a little low, so the face is centred

// The name plate, straddling the window's top edge above the badge.
const float NAME_SIZE   = 26.0f;
const float NAME_HEIGHT = 40.0f;
const float NAME_PAD    = 16.0f;

// Dialogue text.
const float TEXT_SIZE   = 34.0f;
const float TEXT_LINE   = 38.0f;
const float TEXT_GAP    = 22.0f;       // between the badge and the text
const float CHOICE_INDENT = 34.0f;     // room for the cursor

// HUD.
const float HUD_TEXT   = 26.0f;
const float HUD_LINE   = 30.0f;
const float HUD_MARGIN = 16.0f;
const float HUD_PAD    = 14.0f;
const float HINT_Y     = 46.0f;        // centre of the "talk to" hint
const float TOAST_Y    = 664.0f;       // centre of a toast

// Draw order within the overlay (higher is in front).
const float Z_FRAME = 0.0f, Z_INSET = 1.0f, Z_PICTURE = 2.0f, Z_TEXT = 3.0f;

// Colours.
const Vec4 COL_WHITE    = {1.00f, 1.00f, 1.00f, 1.0f};
const Vec4 COL_TEXT     = {0.96f, 0.97f, 1.00f, 1.0f};
const Vec4 COL_DIM      = {0.72f, 0.76f, 0.92f, 1.0f};
const Vec4 COL_PICK     = {1.00f, 0.90f, 0.42f, 1.0f};
const Vec4 COL_NAME     = {1.00f, 0.92f, 0.60f, 1.0f};
// The badge reuses the window frame, tinted brighter (a tint above 1 lightens).
const Vec4 COL_BADGE    = {2.6f, 2.5f, 1.9f, 1.0f};
const Vec4 COL_HUD      = {1.0f, 1.0f, 1.0f, 0.86f};

// Overlay groups (see UiRun::groups).
const char* G_BOX    = "dialogue.box";
const char* G_TEXT   = "dialogue.text";
const char* G_FOOT   = "dialogue.foot";
const char* G_INV    = "hud.inventory";
const char* G_HINT   = "hud.hint";
const char* G_TOAST  = "hud.toast";

// --- Overlay building ------------------------------------------------------------

// Remove every instance of a group from the screen.
void uiClear(SimState& state, const char* group, Diff& out) {
    auto it = state.ui.groups.find(group);
    if (it == state.ui.groups.end()) return;
    for (EntityId id : it->second) out.removals.push_back(id);
    state.ui.groups.erase(it);
}

// Put one instance on the overlay, under a group. Ids are fresh every time, so
// a group replaced within one tick never trips over the Diff's
// upserts-before-removals order.
void uiAdd(SimState& state, const char* group, Instance inst, Diff& out) {
    inst.layer = LAYER_OVERLAY;
    EntityId id = state.nextDynamicId++;
    out.upserts.emplace_back(id, inst);
    state.ui.groups[group].push_back(id);
}

void uiQuad(SimState& state, const char* group, int sheet, int frame,
            float cx, float cy, float w, float h, float z, Vec4 tint, Diff& out) {
    Instance q = makeQuad({cx, cy, z}, {w, h}, 0.0f, 0.0f);
    q.sheet = static_cast<float>(sheet);
    q.tint = tint;
    setAnimation(q, frame, 1, 0.0f, 0.0f);
    uiAdd(state, group, q, out);
}

// A window frame of any size: corners at their natural size, edges and centre
// stretched between them.
void uiFrame(const Scene& scene, SimState& state, const char* group,
             float x0, float y0, float x1, float y1, float z, Vec4 tint, Diff& out) {
    const float c = FRAME_CORNER;
    const float xs[3] = {x0 + c * 0.5f, (x0 + x1) * 0.5f, x1 - c * 0.5f};
    const float ys[3] = {y1 - c * 0.5f, (y0 + y1) * 0.5f, y0 + c * 0.5f};   // top row first
    const float ws[3] = {c, x1 - x0 - 2.0f * c, c};
    const float hs[3] = {c, y1 - y0 - 2.0f * c, c};
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col) {
            if (ws[col] <= 0.0f || hs[row] <= 0.0f) continue;
            uiQuad(state, group, scene.ui.sheet, UI_FRAME_FIRST + row * 3 + col,
                   xs[col], ys[row], ws[col], hs[row], z, tint, out);
        }
}

void uiText(SimState& state, const char* group, const TextBlock& block, Diff& out) {
    for (const Instance& g : block.glyphs) uiAdd(state, group, g, out);
}

// A one-line message in its own small window, centred on (cx, cy).
void uiMessage(const Scene& scene, SimState& state, const char* group,
               const std::string& text, float cx, float cy, Vec4 colour, Diff& out) {
    const float w = textWidth(text, HUD_TEXT);
    const float halfW = w * 0.5f + HUD_PAD + 4.0f;
    const float halfH = HUD_LINE * 0.5f + 8.0f;
    uiFrame(scene, state, group, cx - halfW, cy - halfH, cx + halfW, cy + halfH,
            Z_FRAME, COL_HUD, out);
    uiText(state, group,
           makeTextBlock(text, cx - w * 0.5f, cy + HUD_TEXT * 0.5f, HUD_TEXT, 4096.0f,
                         HUD_LINE, Z_TEXT, scene.ui.fontSheet, colour),
           out);
}

// --- Conversation helpers -------------------------------------------------------

const Dialogue* dialogueOf(const Scene& scene, const std::string& id) {
    auto it = scene.dialogues.find(id);
    return it == scene.dialogues.end() ? nullptr : &it->second;
}

// The first line at or after `from` whose filter passes, or -1.
int nextLine(const SimState& state, const Dialogue& dlg, int from) {
    for (int i = from; i < static_cast<int>(dlg.lines.size()); ++i)
        if (conditionPasses(state, dlg.lines[i].condition)) return i;
    return -1;
}

const Speaker* speakerOf(const Scene& scene, const DialogueLine& line) {
    if (line.speaker.empty()) return nullptr;
    auto it = scene.speakers.find(line.speaker);
    return it == scene.speakers.end() ? nullptr : &it->second;
}

// Where the line's text starts and how wide it may run.
void textArea(bool hasBadge, float& x, float& maxWidth) {
    x = hasBadge ? BADGE_X0 + BADGE_SIZE + TEXT_GAP : BOX_X0 + BOX_PAD + 6.0f;
    maxWidth = BOX_X1 - BOX_PAD - x;
}

TextBlock layoutLine(const Scene& scene, const DialogueLine& line) {
    float x, maxWidth;
    textArea(speakerOf(scene, line) != nullptr, x, maxWidth);
    return makeTextBlock(line.text, x, BOX_Y1 - BOX_PAD, TEXT_SIZE, maxWidth, TEXT_LINE,
                         Z_TEXT, scene.ui.fontSheet, COL_TEXT);
}

// Give the partner its talking animation / put the old one back.
void setPartnerChatting(const Scene& scene, SimState& state, bool chatting, Diff& out) {
    DialogueRun& run = state.talk;
    auto it = state.entities.find(run.partner);
    if (run.partner == 0 || it == state.entities.end()) return;
    if (chatting) {
        run.partnerAnim = it->second.anim;
        const Clip* clip = scene.clipOf(run.partner, "chat");
        if (!clip) return;
        setAnimation(it->second, clip->first, clip->count, clip->fps,
                     static_cast<float>(state.clock.time()));
    } else {
        it->second.anim = run.partnerAnim;
    }
    out.upserts.emplace_back(run.partner, it->second);
}

void closeDialogue(const Scene& scene, SimState& state, Diff& out) {
    uiClear(state, G_BOX, out);
    uiClear(state, G_TEXT, out);
    uiClear(state, G_FOOT, out);
    setPartnerChatting(scene, state, false, out);
    state.talk = DialogueRun{};
}

// Bring line `index` up: apply what it does, draw the window, badge and name.
// The text itself is typed out tick by tick in stepDialogue.
void enterLine(const Scene& scene, SimState& state, const Dialogue& dlg, int index, Diff& out) {
    DialogueRun& run = state.talk;
    const DialogueLine& line = dlg.lines[index];
    run.line = index;
    run.lineTick = state.clock.tick;
    run.shown = 0;
    run.revealed = false;
    run.footBuilt = false;
    run.choice = 0;

    for (const auto& kv : line.set)  state.flags[kv.first] = kv.second;
    for (const auto& kv : line.take) giveItem(scene, state, kv.first, -static_cast<int>(kv.second), out);
    for (const auto& kv : line.give) giveItem(scene, state, kv.first,  static_cast<int>(kv.second), out);

    uiClear(state, G_BOX, out);
    uiClear(state, G_TEXT, out);
    uiClear(state, G_FOOT, out);
    uiFrame(scene, state, G_BOX, BOX_X0, BOX_Y0, BOX_X1, BOX_Y1, Z_FRAME, COL_WHITE, out);

    if (const Speaker* who = speakerOf(scene, line)) {
        const float bx1 = BADGE_X0 + BADGE_SIZE, by1 = BADGE_Y0 + BADGE_SIZE;
        uiFrame(scene, state, G_BOX, BADGE_X0, BADGE_Y0, bx1, by1, Z_INSET, COL_BADGE, out);

        // The face: the speaker's own sprite, animated by the shader as usual.
        Instance face = makeQuad({(BADGE_X0 + bx1) * 0.5f,
                                  (BADGE_Y0 + by1) * 0.5f - PORTRAIT_DROP, Z_PICTURE},
                                 {PORTRAIT_SIZE, PORTRAIT_SIZE}, 0.0f, 0.0f);
        face.sheet = static_cast<float>(who->sheet);
        face.tint = who->tint;
        setAnimation(face, who->first, who->count, who->fps,
                     static_cast<float>(state.clock.time()));
        uiAdd(state, G_BOX, face, out);

        const float nameW = textWidth(who->name, NAME_SIZE);
        const float nx0 = BADGE_X0, nx1 = nx0 + nameW + 2.0f * NAME_PAD;
        const float ny0 = BOX_Y1 - NAME_HEIGHT * 0.45f, ny1 = ny0 + NAME_HEIGHT;
        uiFrame(scene, state, G_BOX, nx0, ny0, nx1, ny1, Z_INSET, COL_WHITE, out);
        uiText(state, G_BOX,
               makeTextBlock(who->name, nx0 + NAME_PAD, (ny0 + ny1) * 0.5f + NAME_SIZE * 0.5f,
                             NAME_SIZE, 4096.0f, NAME_SIZE, Z_TEXT, scene.ui.fontSheet, COL_NAME),
               out);
    }
}

// Under a finished line: its choices with the cursor, or the "more" marker.
void buildFoot(const Scene& scene, SimState& state, const DialogueLine& line,
               int textLines, Diff& out) {
    DialogueRun& run = state.talk;
    uiClear(state, G_FOOT, out);
    run.footBuilt = true;

    if (line.choices.empty()) {
        Instance more = makeQuad({BOX_X1 - BOX_PAD - 10.0f, BOX_Y0 + BOX_PAD + 4.0f, Z_TEXT},
                                 {24.0f, 24.0f}, 0.0f, 0.0f);
        more.sheet = static_cast<float>(scene.ui.sheet);
        more.tint = COL_DIM;
        setAnimation(more, UI_MORE, 2, MORE_BLINK_FPS, static_cast<float>(state.clock.time()));
        uiAdd(state, G_FOOT, more, out);
        return;
    }

    float x, maxWidth;
    textArea(speakerOf(scene, line) != nullptr, x, maxWidth);
    for (int i = 0; i < static_cast<int>(line.choices.size()); ++i) {
        const bool picked = (i == run.choice);
        const float top = BOX_Y1 - BOX_PAD - (textLines + i) * TEXT_LINE;
        if (picked)
            uiQuad(state, G_FOOT, scene.ui.sheet, UI_CURSOR, x + 12.0f, top - TEXT_SIZE * 0.5f,
                   24.0f, 24.0f, Z_TEXT, COL_PICK, out);
        uiText(state, G_FOOT,
               makeTextBlock(line.choices[i].text, x + CHOICE_INDENT, top, TEXT_SIZE,
                             maxWidth - CHOICE_INDENT, TEXT_LINE, Z_TEXT, scene.ui.fontSheet,
                             picked ? COL_PICK : COL_DIM),
               out);
    }
}

// Which key to name in the hint: space if it is bound to interact, else the
// alphabetically first key that is.
std::string interactKey(const Scene& scene) {
    std::string best;
    for (const auto& kv : scene.bindings) {
        if (kv.second != ACT_INTERACT) continue;
        if (kv.first == "space") { best = kv.first; break; }
        if (best.empty() || kv.first < best) best = kv.first;
    }
    for (char& c : best) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return best;
}

// The nearest NPC with something to say within reach of the player, or 0.
// Ties go to the lower id, so the choice never depends on iteration order.
EntityId talkTarget(const Scene& scene, const SimState& state,
                    const std::unordered_map<EntityId, Vec3>& positions) {
    auto me = positions.find(playerOf(scene, state));
    if (me == positions.end()) return 0;
    EntityId best = 0;
    float bestDist2 = TALK_REACH * TALK_REACH;
    for (EntityId id : scene.talkIds) {
        auto p = positions.find(id);
        if (p == positions.end()) continue;
        const float dx = p->second.x - me->second.x, dz = p->second.z - me->second.z;
        const float d2 = dx * dx + dz * dz;
        if (d2 < bestDist2) { bestDist2 = d2; best = id; }
    }
    return best;
}

} // namespace

EntityId playerOf(const Scene& scene, const SimState& state) {
    if (scene.rigFollow != 0 && state.entities.count(scene.rigFollow)) return scene.rigFollow;
    EntityId best = 0;
    for (const auto& kv : state.attrs)
        if (kv.second.controlled && (best == 0 || kv.first < best)) best = kv.first;
    return best;
}

void giveItem(const Scene& scene, SimState& state, const std::string& item,
              int count, Diff& out) {
    if (count == 0) return;
    int& have = state.inventory[item];
    have += count;
    if (have <= 0) state.inventory.erase(item);
    state.ui.inventoryDirty = true;
    if (count > 0)
        showToast(scene, state, "+" + std::to_string(count) + " " + scene.itemName(item), out);
}

void showToast(const Scene& scene, SimState& state, const std::string& text, Diff& out) {
    if (!scene.ui.enabled) return;
    uiClear(state, G_TOAST, out);
    uiMessage(scene, state, G_TOAST, text, OVERLAY_WIDTH * 0.5f, TOAST_Y, COL_PICK, out);
    state.ui.toastUntil = state.clock.tick + TOAST_TICKS;
}

void startDialogue(const Scene& scene, SimState& state, const std::string& id,
                   EntityId partner, Diff& out) {
    const Dialogue* dlg = dialogueOf(scene, id);
    if (!dlg || !scene.ui.enabled) return;
    const int first = nextLine(state, *dlg, 0);
    if (first < 0) return;

    if (state.talk.active) closeDialogue(scene, state, out);
    state.talk.active = true;
    state.talk.id = id;
    state.talk.partner = partner;
    setPartnerChatting(scene, state, true, out);
    uiClear(state, G_HINT, out);
    state.ui.hintTarget = 0;
    enterLine(scene, state, *dlg, first, out);
}

void stepDialogue(const Scene& scene, SimState& state,
                  const ResolvedActions& actions, Diff& out) {
    DialogueRun& run = state.talk;
    const Dialogue* dlg = dialogueOf(scene, run.id);
    if (!run.active || !dlg || run.line < 0) { run = DialogueRun{}; return; }
    const DialogueLine& line = dlg->lines[run.line];
    const TextBlock block = layoutLine(scene, line);
    const int total = static_cast<int>(block.glyphs.size());

    const bool interact = actions.pressed.count(ACT_INTERACT) != 0;

    if (run.revealed && run.footBuilt) {
        // The line is out: move the cursor, or confirm.
        const int n = static_cast<int>(line.choices.size());
        if (n > 0) {
            int move = 0;
            if (actions.pressed.count("move_north")) move -= 1;
            if (actions.pressed.count("move_south")) move += 1;
            if (move != 0) {
                run.choice = (run.choice + move + n) % n;
                buildFoot(scene, state, line, block.lines, out);
            }
        }
        if (interact) {
            if (n > 0)
                for (const auto& kv : line.choices[run.choice].set) state.flags[kv.first] = kv.second;
            const int next = line.end ? -1 : nextLine(state, *dlg, run.line + 1);
            if (next < 0) closeDialogue(scene, state, out);
            else          enterLine(scene, state, *dlg, next, out);
        }
        return;
    }

    // Still typing: the interact action finishes the line at once.
    int target = total;
    if (!interact) {
        const uint64_t elapsed = state.clock.tick - run.lineTick;
        const uint64_t typed = elapsed * REVEAL_GLYPHS / REVEAL_TICKS + 1;
        if (typed < static_cast<uint64_t>(total)) target = static_cast<int>(typed);
    }
    for (int i = run.shown; i < target; ++i) uiAdd(state, G_TEXT, block.glyphs[i], out);
    run.shown = target;
    if (run.shown >= total) {
        run.revealed = true;
        buildFoot(scene, state, line, block.lines, out);
    }
}

void stepInteractions(const Scene& scene, SimState& state,
                      const ResolvedActions& actions,
                      const std::unordered_map<EntityId, Vec3>& positions,
                      Diff& out) {
    auto me = positions.find(playerOf(scene, state));
    if (me == positions.end()) return;

    // Pickups: walking into one collects it.
    for (EntityId id : scene.pickupIds) {
        auto p = positions.find(id);
        if (p == positions.end() || !state.entities.count(id)) continue;
        const float dx = p->second.x - me->second.x, dz = p->second.z - me->second.z;
        if (dx * dx + dz * dz > PICKUP_REACH * PICKUP_REACH) continue;
        const Interactable& what = scene.interactables.at(id);
        removeEntity(scene, state, id, out);
        giveItem(scene, state, what.item, what.count, out);
    }

    if (actions.pressed.count(ACT_INTERACT)) {
        if (EntityId who = talkTarget(scene, state, positions))
            startDialogue(scene, state, scene.interactables.at(who).talk, who, out);
    }
}

void refreshHud(const Scene& scene, SimState& state,
                const std::unordered_map<EntityId, Vec3>& positions, Diff& out) {
    if (!scene.ui.enabled) return;

    // Toast: gone when its time is up.
    if (state.ui.toastUntil != 0 && state.clock.tick >= state.ui.toastUntil) {
        uiClear(state, G_TOAST, out);
        state.ui.toastUntil = 0;
    }

    // Inventory: one line per item, in a window sized to fit, top-left.
    if (state.ui.inventoryDirty) {
        state.ui.inventoryDirty = false;
        uiClear(state, G_INV, out);
        if (!state.inventory.empty()) {
            std::vector<std::string> rows;
            float widest = 0.0f;
            for (const auto& kv : state.inventory) {
                rows.push_back(scene.itemName(kv.first) + "  x" + std::to_string(kv.second));
                const float w = textWidth(rows.back(), HUD_TEXT);
                if (w > widest) widest = w;
            }
            const float x0 = HUD_MARGIN, y1 = OVERLAY_HEIGHT - HUD_MARGIN;
            const float x1 = x0 + widest + 2.0f * HUD_PAD + 8.0f;
            const float y0 = y1 - rows.size() * HUD_LINE - 2.0f * HUD_PAD;
            uiFrame(scene, state, G_INV, x0, y0, x1, y1, Z_FRAME, COL_HUD, out);
            for (size_t i = 0; i < rows.size(); ++i)
                uiText(state, G_INV,
                       makeTextBlock(rows[i], x0 + HUD_PAD + 4.0f,
                                     y1 - HUD_PAD - i * HUD_LINE - (HUD_LINE - HUD_TEXT) * 0.5f,
                                     HUD_TEXT, 4096.0f, HUD_LINE, Z_TEXT,
                                     scene.ui.fontSheet, COL_TEXT),
                       out);
        }
    }

    // Hint: who the interact action would talk to right now, if anyone.
    const EntityId target = state.talk.active ? 0 : talkTarget(scene, state, positions);
    if (target != state.ui.hintTarget) {
        state.ui.hintTarget = target;
        uiClear(state, G_HINT, out);
        if (target != 0) {
            const std::string key = interactKey(scene);
            const std::string& name = scene.interactables.at(target).name;
            uiMessage(scene, state, G_HINT,
                      (key.empty() ? std::string() : key + ":  ") + "Talk to " + name,
                      OVERLAY_WIDTH * 0.5f, HINT_Y, COL_TEXT, out);
        }
    }
}

} // namespace cv
