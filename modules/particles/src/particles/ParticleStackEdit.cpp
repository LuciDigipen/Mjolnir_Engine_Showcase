/**
 * @file ParticleStackEdit.cpp
 * @author sumin.park
 * @brief Add, remove and reorder edits on an emitter's behavior stacks.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "particles/ParticleStackEdit.h"

#include <algorithm>
#include <utility>

namespace mir
{
    namespace
    {
        constexpr size_t kMaxStackEntries = 32;

        std::vector<ParticleBehavior> &Stack(ParticleEmitterAsset &emitter, bool spawn)
        {
            return spawn ? emitter.spawnBehaviors : emitter.updateBehaviors;
        }

        const ParticleBehaviorInfo *Find(const ParticleEmitterAsset &emitter, bool spawn, std::string_view name)
        {
            const auto infos = ParticleBehaviors();
            const auto found = std::find_if(infos.begin(), infos.end(), [&](const ParticleBehaviorInfo &info)
                {
                    return info.name == name && info.spawn == spawn && info.target == emitter.simulationTarget;
                });
            return found == infos.end() ? nullptr : &*found;
        }
    }

    bool AddParticleBehavior(ParticleEmitterAsset &emitter, bool spawn, std::string_view name)
    {
        auto &stack = Stack(emitter, spawn);
        const auto *info = Find(emitter, spawn, name);
        if (!info || stack.size() >= kMaxStackEntries) return false;
        if (emitter.sourceVersion >= 3 && std::any_of(stack.begin(), stack.end(),
            [&](const ParticleBehavior &entry) { return entry.name == name; })) return false;
        ParticleBehavior behavior{ std::string(info->name),{} };
        for (const auto &parameter : info->parameters) behavior.parameters.push_back(parameter.defaultValue);
        if (emitter.sourceVersion >= 3 && !info->group.empty())
            for (auto &entry : stack)
            {
                const auto *other = Find(emitter, spawn, entry.name);
                if (other && other->group == info->group) entry.enabled = false;
            }
        stack.push_back(std::move(behavior));
        return true;
    }

    bool CanRemoveParticleBehavior(const ParticleEmitterAsset &emitter, bool spawn, size_t index)
    {
        const auto &stack = spawn ? emitter.spawnBehaviors : emitter.updateBehaviors;
        if (index >= stack.size()) return false;
        const auto *info = Find(emitter, spawn, stack[index].name);
        if (!info || !info->required) return true;
        if (emitter.sourceVersion >= 3) return false;
        return std::count_if(stack.begin(), stack.end(), [&](const ParticleBehavior &entry) { return entry.name == stack[index].name; }) > 1;
    }

    bool RemoveParticleBehavior(ParticleEmitterAsset &emitter, bool spawn, size_t index)
    {
        if (!CanRemoveParticleBehavior(emitter, spawn, index)) return false;
        auto &stack = Stack(emitter, spawn);
        stack.erase(stack.begin() + static_cast<std::ptrdiff_t>(index));
        return true;
    }

    bool MoveParticleBehavior(ParticleEmitterAsset &emitter, bool spawn, size_t index, int direction)
    {
        if (emitter.sourceVersion >= 3) return false;
        auto &stack = Stack(emitter, spawn);
        if (index >= stack.size() || (direction < 0 && index == 0) || (direction > 0 && index + 1 >= stack.size())) return false;
        std::swap(stack[index], stack[direction < 0 ? index - 1 : index + 1]);
        return true;
    }

    bool SetParticleBehaviorEnabled(ParticleEmitterAsset &emitter, bool spawn, size_t index, bool enabled)
    {
        if (emitter.sourceVersion < 3) return false;
        auto &stack = Stack(emitter, spawn);
        if (index >= stack.size()) return false;
        const auto *info = Find(emitter, spawn, stack[index].name);
        if (!info || (info->required && !enabled) || stack[index].enabled == enabled) return false;
        if (enabled && !info->group.empty())
            for (auto &other : stack)
            {
                const auto *candidate = Find(emitter, spawn, other.name);
                if (candidate && candidate->group == info->group) other.enabled = false;
            }
        stack[index].enabled = enabled;
        return true;
    }

    bool UpgradeParticleEmitter(ParticleEmitterAsset &emitter, std::string &message)
    {
        if (emitter.sourceVersion >= 3) { message.clear(); return true; }
        auto upgraded = emitter;
        upgraded.sourceVersion = 3;
        if (!ValidateParticleEmitterAsset(upgraded, message)) return false;
        bool reordered = false;
        for (bool spawn : { true, false })
        {
            const auto &old = spawn ? emitter.spawnBehaviors : emitter.updateBehaviors;
            const auto ordered = ActiveParticleBehaviors(upgraded, spawn);
            for (size_t i = 0; i < ordered.size(); ++i)
                if (ordered[i]->name != old[i].name) reordered = true;
        }
        const auto has = [&](std::string_view name)
            {
                return std::any_of(emitter.updateBehaviors.begin(), emitter.updateBehaviors.end(),
                    [&](const ParticleBehavior &behavior) { return behavior.name == name; });
            };
        const bool opacityChanges = (has("ColorGradient") && has("FadeOverLife")) ||
            (has("FadeOverLife") && std::any_of(emitter.updateBehaviors.begin(), emitter.updateBehaviors.end(),
                [](const ParticleBehavior &behavior)
                {
                    return behavior.name == "FadeOverLife" && behavior.parameters[0] != 0;
                }));
        const bool ribbonOrderChanges = has("RibbonColorGradient") && has("RibbonFade") && reordered;
        emitter = std::move(upgraded);
        message = reordered || opacityChanges || ribbonOrderChanges ?
            "Fixed behavior order or opacity composition may change the effect, compare the preview before saving." : "";
        return true;
    }
}
