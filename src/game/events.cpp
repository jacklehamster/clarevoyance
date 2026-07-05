// events.cpp — event/condition/action evaluation over (const Scene, SimState).
#include "events.h"

#include <cstdio>
#include <cmath>

namespace cv {

static double flagValue(const SimState& state, const std::string& name) {
    auto it = state.flags.find(name);
    return it == state.flags.end() ? 0.0 : it->second;
}

// Recursive: a condition is a flag comparison or an all/any composition.
static bool conditionPasses(const SimState& state, const Condition& c) {
    switch (c.kind) {
        case Condition::Kind::None:
            return true;

        case Condition::Kind::Flag: {
            const double v = flagValue(state, c.flag);
            switch (c.op) {
                case Condition::Op::Eq: return v == c.value;
                case Condition::Op::Ne: return v != c.value;
                case Condition::Op::Lt: return v <  c.value;
                case Condition::Op::Le: return v <= c.value;
                case Condition::Op::Gt: return v >  c.value;
                case Condition::Op::Ge: return v >= c.value;
            }
            return false;
        }

        case Condition::Kind::All:
            for (const Condition& child : c.children)
                if (!conditionPasses(state, child)) return false;
            return true;

        case Condition::Kind::Any:
            for (const Condition& child : c.children)
                if (conditionPasses(state, child)) return true;
            return false;
    }
    return true;
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

            case Action::Type::AddFlag:
                state.flags[a.flag] += a.value;   // missing flag starts at 0
                break;

            case Action::Type::SetAnim: {
                EntityId id = scene.idOf(a.entity);
                if (id == 0) break;
                auto it = state.entities.find(id);
                if (it == state.entities.end()) break;
                int first = a.first, count = a.count;
                float fps = a.fps;
                if (!a.clip.empty()) {
                    // Named clip: resolve through the target's archetype.
                    // loadScene validates data-file clip references, so a miss
                    // here (code-built scene) just keeps the raw fields.
                    if (const Clip* clip = scene.clipOf(id, a.clip)) {
                        first = clip->first; count = clip->count; fps = clip->fps;
                    }
                }
                setAnimation(it->second, first, count, fps, 0.0f);
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
                state.attrs[id].controlled = (a.value != 0.0);
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
