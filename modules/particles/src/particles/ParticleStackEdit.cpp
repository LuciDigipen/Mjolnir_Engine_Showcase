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
                { return info.name == name && info.spawn == spawn && info.target == emitter.simulationTarget; });
            return found == infos.end() ? nullptr : &*found;
        }
    }

    bool AddParticleBehavior(ParticleEmitterAsset &emitter, bool spawn, std::string_view name)
    {
        auto &stack = Stack(emitter, spawn);
        const auto *info = Find(emitter, spawn, name);
        if (!info || stack.size() >= kMaxStackEntries) return false;
        ParticleBehavior behavior{ std::string(info->name),{} };
        for (const auto &parameter : info->parameters) behavior.parameters.push_back(parameter.defaultValue);
        stack.push_back(std::move(behavior));
        return true;
    }

    bool CanRemoveParticleBehavior(const ParticleEmitterAsset &emitter, bool spawn, size_t index)
    {
        const auto &stack = spawn ? emitter.spawnBehaviors : emitter.updateBehaviors;
        if (index >= stack.size()) return false;
        const auto *info = Find(emitter, spawn, stack[index].name);
        if (!info || !info->required) return true;
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
        auto &stack = Stack(emitter, spawn);
        if (index >= stack.size() || (direction < 0 && index == 0) || (direction > 0 && index + 1 >= stack.size())) return false;
        std::swap(stack[index], stack[direction < 0 ? index - 1 : index + 1]);
        return true;
    }
}
