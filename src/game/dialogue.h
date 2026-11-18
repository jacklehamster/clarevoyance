// dialogue.h — talking to NPCs, picking things up, and the screen overlay.
//
// Conversations are old-school: a window along the bottom of the screen, the
// speaker's face in a badge on its left with their name above, the line typed
// out a few letters at a time. The interact action finishes the line, then
// moves on; a line with choices shows them under the text, picked with
// up/down + interact. While a conversation is open the player doesn't move.
//
// How a conversation branches is described in scene.h (DialogueLine): a flat
// list of lines, each with an optional filter over variables (flags) and the
// inventory. A choice sets variables; later lines filter on them.
//
// Objects are simpler still: an entity with a "pickup" is collected by walking
// into it — it goes into the inventory (a count per item id), which the HUD
// lists in the top-left corner.
//
// Everything on screen is built here as ordinary overlay Instances
// (Instance::layer = LAYER_OVERLAY) — window frames, portrait, glyphs. The
// renderer has no notion of "dialogue".
//
// All of it runs inside the fixed-tick sim: reveal timing counts ticks, the
// nearest NPC is chosen by distance then id. Same inputs, same conversation.
#pragma once

#include <string>
#include <unordered_map>

#include "scene.h"
#include "sim.h"

namespace cv {

// Abstract action that talks / advances / confirms. Scenes bind keys to it.
constexpr const char* ACT_INTERACT = "interact";

// How close (in the ground plane) the player must be to talk to someone, and
// to pick something up.
constexpr float TALK_REACH   = 1.45f;
constexpr float PICKUP_REACH = 0.55f;

// The typewriter: REVEAL_GLYPHS glyphs every REVEAL_TICKS ticks (40 per second).
constexpr int REVEAL_GLYPHS = 2;
constexpr int REVEAL_TICKS  = 3;

// A toast stays up this long (2.5 s at 60 Hz).
constexpr int TOAST_TICKS = 150;

// The entity the player is: the one the camera follows, else the first
// controlled entity. 0 if there is none (any more).
EntityId playerOf(const Scene& scene, const SimState& state);

// Open a conversation. `partner` (may be 0) is the entity being talked to.
// Does nothing if the dialogue is unknown or none of its lines passes.
void startDialogue(const Scene& scene, SimState& state, const std::string& id,
                   EntityId partner, Diff& out);

// One tick of an open conversation: input (advance / choose), the typewriter.
void stepDialogue(const Scene& scene, SimState& state,
                  const ResolvedActions& actions, Diff& out);

// One tick of walking around: collect pickups in reach; on the interact
// action, start talking to the nearest NPC in reach.
void stepInteractions(const Scene& scene, SimState& state,
                      const ResolvedActions& actions,
                      const std::unordered_map<EntityId, Vec3>& positions,
                      Diff& out);

// Change the inventory (count may be negative; it never drops below zero).
// A gain announces itself with a toast.
void giveItem(const Scene& scene, SimState& state, const std::string& item,
              int count, Diff& out);

// Flash a short message at the top of the screen.
void showToast(const Scene& scene, SimState& state, const std::string& text, Diff& out);

// Keep the HUD current: the inventory list, the "talk to" hint, toast expiry.
// Cheap when nothing changed — it only rebuilds what did.
void refreshHud(const Scene& scene, SimState& state,
                const std::unordered_map<EntityId, Vec3>& positions, Diff& out);

} // namespace cv
