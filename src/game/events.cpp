// events.cpp — EventSystem implementation.
#include "events.h"
#include "scene.h"

#include <cstdio>
#include <cmath>

namespace cv {

void EventSystem::init(const Scene& scene) {
    flags_.clear();
    firstStep_ = true;
    entities_ = scene.initialState.instances;  // working copy for animation swaps
    attrs_    = scene.attrs;                    // working copy of controlled/speed
}

bool EventSystem::conditionPasses(const Condition& c) const {
    if (!c.present) return true;
    return flag(c.flag) == c.value;
}

void EventSystem::runActions(const Scene& scene, float now,
                             const std::unordered_map<EntityId, Vec3>& positions,
                             const std::vector<Action>& actions, Diff& out) {
    for (const Action& a : actions) {
        switch (a.type) {
            case Action::Type::Dialogue:
                // Engine-agnostic: a sink/UI can grep this prefix. For now it logs.
                std::printf("CV_DIALOGUE: %s\n", a.id.c_str());
                break;

            case Action::Type::SetFlag:
                flags_[a.flag] = a.value;
                break;

            case Action::Type::SetAnim: {
                EntityId id = scene.idOf(a.entity);
                if (id == 0) break;
                auto it = entities_.find(id);
                if (it == entities_.end()) break;
                setAnimation(it->second, a.first, a.count, a.fps, 0.0f);
                out.upserts.emplace_back(id, it->second);
                break;
            }

            case Action::Type::SetMotion: {
                EntityId id = scene.idOf(a.entity);
                if (id == 0) break;
                auto it = entities_.find(id);
                if (it == entities_.end()) break;
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
                entities_.erase(id);
                attrs_.erase(id);
                out.removals.push_back(id);
                break;
            }

            case Action::Type::ToggleControlled: {
                EntityId id = scene.idOf(a.entity);
                if (id == 0) break;
                attrs_[id].controlled = !attrs_[id].controlled;
                break;
            }

            case Action::Type::SetControlled: {
                EntityId id = scene.idOf(a.entity);
                if (id == 0) break;
                attrs_[id].controlled = a.value;
                break;
            }
        }
    }
}

void EventSystem::update(Scene& scene,
                         float now,
                         const std::unordered_map<EntityId, Vec3>& positions,
                         const ResolvedActions& actions,
                         Diff& out) {
    const bool isFirstStep = firstStep_;
    firstStep_ = false;

    for (Event& e : scene.events) {
        if (e.fired && e.once) continue;
        if (!conditionPasses(e.condition)) continue;

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
                // this frame (pressed/released), or is currently held.
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
            runActions(scene, now, positions, e.actions, out);
            e.fired = true;
        }
    }
}

} // namespace cv
