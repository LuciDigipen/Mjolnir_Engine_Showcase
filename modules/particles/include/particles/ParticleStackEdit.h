/**
 * @file ParticleStackEdit.h
 * @author sumin.park
 * @brief Add, remove and reorder edits on an emitter's behavior stacks, shared by the editor and its tests.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "particles/ParticleEffect.h"

#include <cstddef>
#include <string_view>

namespace mir
{
    // The new entry gets the registry defaults, false when the name is unknown for this stage and target or the stack is full.
    bool AddParticleBehavior(ParticleEmitterAsset &emitter, bool spawn, std::string_view name);
    // The last copy of a required behavior stays, because the emitter is invalid without it.
    bool CanRemoveParticleBehavior(const ParticleEmitterAsset &emitter, bool spawn, size_t index);
    bool RemoveParticleBehavior(ParticleEmitterAsset &emitter, bool spawn, size_t index);
    // New authoring keeps one entry per kind and remembers disabled parameter values.
    bool SetParticleBehaviorEnabled(ParticleEmitterAsset &emitter, bool spawn, size_t index, bool enabled);
    // Explicitly converts an ordered legacy emitter to fixed authoring rules.
    bool UpgradeParticleEmitter(ParticleEmitterAsset &emitter, std::string &message);
    // direction is -1 to move toward the front and 1 toward the back, false at either end.
    bool MoveParticleBehavior(ParticleEmitterAsset &emitter, bool spawn, size_t index, int direction);
}
