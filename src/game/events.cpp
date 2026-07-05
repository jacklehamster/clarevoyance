// events.cpp — event/condition/action evaluation over (const Scene, SimState).
#include "events.h"

#include <cstdio>
#include <cmath>

namespace cv {

static bool flagValue(const SimState& state, const std::string& name) {
    auto it = state.flags.find(name);
    return it != state.flags.end() && it->second;
}

static bool conditionPasses(const SimState& state, const Condition& c) {
    if (!c.present) return true;
    return flagValue(state, c.flag) == c.value;
}

static void runActions(const Scene& scene, SimState& state, float now,
                       const std::unordered_map<EntityId, Vec3>& positions,
                       const std::vector<Action>& actions, Diff& out) {
    for (const Action& a : actions) {
        switch (a.type) {
            case Action::Type::Dialogue:
                // Engine-agnostic: a sink/UI can grep this prefix. For now it logs.
                std::printf("CV_DIALOGUE: %s\n", a.id.c_str());
                break;

            case Action::Type::SetFlag:
                state.flags[a.flag] = a.value;
                break;

            case Action::Type::SetAnim: {
                EntityId id = scene.idOf(a.entity);
                if (id == 0) break;
                auto it = state.entities.find(id);
                if (it == state.entities.end()) break;
                setAnimation(it->second, a.first, a.count, a.fps, 0.0f);
                out.upserts.emplace_back(id, it->second);
                break;
            }

            case Action::Type::SetMotion: {
                EntityId id = scene.idOf(a.entity);
                if (id == 0) break;
                auto it = state.entities.find(id);
                if (it == state.entities.end()) break;
                // Rebase the motion origin to the entity's current position so it
                // continues from where it is rather than teleporting.
                auto p = positions.find(id);
                if (p != positions.end()) it->second.pos = p->second;
                setMotion(it->second, a.vel, a.accel, now);
                out.upserts.emplace_back(id, it->second);
                break;
            }

            case Action::Type::Remove: {
                EntityId id = scene.idOf(a.entity);
                if (id == 0) break;
                state.entities.erase(id);
                state.attrs.erase(id);
                out.removals.push_back(id);
                break;
            }

            case Action::Type::ToggleControlled: {
                EntityId id = scene.idOf(a.entity);
                if (id == 0) break;
                state.attrs[id].controlled = !state.attrs[id].controlled;
                break;
            }

            case Action::Type::SetControlled: {
                EntityId id = scene.idOf(a.entity);
                if (id == 0) break;
                state.attrs[id].controlled = a.value;
                break;
            }
        }
    }
}

void updateEvents(const Scene& scene,
                  SimState& state,
                  double now,
                  const std::unordered_map<EntityId, Vec3>& positions,
                  const ResolvedActions& actions,
                  Diff& out) {
    const bool isFirstStep = !state.started;
    state.started = true;

    // Defensive: keep the fired markers sized to the scene's event list
    // (makeSimState sizes them; this guards a mismatched state/scene pair).
    if (state.fired.size() != scene.events.size())
        state.fired.assign(scene.events.size(), 0);

    const float nowF = static_cast<float>(now);

    for (size_t i = 0; i < scene.events.size(); ++i) {
        const Event& e = scene.events[i];
        if (state.fired[i] && e.once) continue;
        if (!conditionPasses(state, e.condition)) continue;

        bool triggered = false;
        switch (e.trigger.type) {
            case Trigger::Type::Start:
                triggered = isFirstStep;
                break;

            case Trigger::Type::Proximity: {
                EntityId a = scene.idOf(e.trigger.entity);
                EntityId b = scene.idOf(e.trigger.target);
                auto pa = positions.find(a);
                auto pb = positions.find(b);
                if (a == 0 || b == 0 || pa == positions.end() || pb == positions.end())
                    break;
                Vec3 d = pa->second - pb->second;
                float dist2 = dot(d, d);
                float r = e.trigger.radius;
                triggered = dist2 <= r * r;
                break;
            }

            case Trigger::Type::Input: {
                // Fire when the named abstract action shows the requested edge
                // this tick (pressed/released), or is currently held.
                const std::string& act = e.trigger.action;
                switch (e.trigger.edge) {
                    case Trigger::Edge::Pressed:
                        triggered = actions.pressed.count(act) != 0; break;
                    case Trigger::Edge::Released:
                        triggered = actions.released.count(act) != 0; break;
                    case Trigger::Edge::Held:
                        triggered = actions.down.count(act) != 0; break;
                }
                break;
            }
        }

        if (triggered) {
            runActions(scene, state, nowF, positions, e.actions, out);
            state.fired[i] = 1;
        }
    }
}

} // namespace cv
