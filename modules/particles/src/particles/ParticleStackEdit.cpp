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
        if (std::any_of(stack.begin(), stack.end(),
            [&](const ParticleBehavior &entry) { return entry.name == name; })) return false;
        ParticleBehavior behavior{ std::string(info->name),{} };
        for (const auto &parameter : info->parameters) behavior.parameters.push_back(parameter.defaultValue);
        if (!info->group.empty())
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
        return !info || !info->required;
    }

    bool RemoveParticleBehavior(ParticleEmitterAsset &emitter, bool spawn, size_t index)
    {
        if (!CanRemoveParticleBehavior(emitter, spawn, index)) return false;
        auto &stack = Stack(emitter, spawn);
        stack.erase(stack.begin() + static_cast<std::ptrdiff_t>(index));
        return true;
    }

    bool SetParticleBehaviorEnabled(ParticleEmitterAsset &emitter, bool spawn, size_t index, bool enabled)
    {
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
}
