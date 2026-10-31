/**
 * @file ParticleSystem.h
 * @author sumin.park
 * @brief Component effect reference and playback state.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "particles/ParticleRuntime.h"

#include <core/ecs/ComponentAsserts.h>
#include <core/ecs/ComponentFields.h>

#include <cstddef>

namespace mir
{
    struct ParticleSystem
    {
        char systemName[kFieldStringCapacity] = {};
        bool playOnStart = true;
        bool overrideParameters = false;
        ParticleUserParameters userParameters;
        // Per-instance origin of emitter placements. A slot belongs to the placement whose id is in the same slot of
        // offsetPlacementIds, 0 meaning unused, so reordering or removing placements in the system never moves an offset
        // onto another emitter.
        uint32_t offsetPlacementIds[kMaxSystemEmitters] = {};
        glm::vec3 emitterOffsets[kMaxSystemEmitters] = {};
        ParticleHandle instance;
        bool initialized = false;
        bool wantsPlay = false;
        bool paused = false;
        AssetId failedEffect;
    };
    MIR_ASSERT_COMPONENT(ParticleSystem);

    inline constexpr FieldDesc kParticleSystemFields[] = {
        { "systemName", FieldKind::String, offsetof(ParticleSystem, systemName) },
        { "playOnStart", FieldKind::Bool, offsetof(ParticleSystem, playOnStart) },
        { "overrideParameters", FieldKind::Bool, offsetof(ParticleSystem, overrideParameters) },
        { "wind", FieldKind::Vec3, offsetof(ParticleSystem, userParameters) + offsetof(ParticleUserParameters, wind) },
        { "spawnRateScale", FieldKind::Float, offsetof(ParticleSystem, userParameters) + offsetof(ParticleUserParameters, spawnRateScale) },
        { "sizeScale", FieldKind::Float, offsetof(ParticleSystem, userParameters) + offsetof(ParticleUserParameters, sizeScale) },
        // Edited in the particle panel's emitter list, so the inspector hides them.
        { .mName = "emitterOffset0", .mKind = FieldKind::Vec3, .mOffset = offsetof(ParticleSystem, emitterOffsets), .mHidden = true },
        { .mName = "emitterOffset1", .mKind = FieldKind::Vec3, .mOffset = offsetof(ParticleSystem, emitterOffsets) + 1 * sizeof(glm::vec3), .mHidden = true },
        { .mName = "emitterOffset2", .mKind = FieldKind::Vec3, .mOffset = offsetof(ParticleSystem, emitterOffsets) + 2 * sizeof(glm::vec3), .mHidden = true },
        { .mName = "emitterOffset3", .mKind = FieldKind::Vec3, .mOffset = offsetof(ParticleSystem, emitterOffsets) + 3 * sizeof(glm::vec3), .mHidden = true },
        { .mName = "emitterOffset4", .mKind = FieldKind::Vec3, .mOffset = offsetof(ParticleSystem, emitterOffsets) + 4 * sizeof(glm::vec3), .mHidden = true },
        { .mName = "emitterOffset5", .mKind = FieldKind::Vec3, .mOffset = offsetof(ParticleSystem, emitterOffsets) + 5 * sizeof(glm::vec3), .mHidden = true },
        { .mName = "emitterOffset6", .mKind = FieldKind::Vec3, .mOffset = offsetof(ParticleSystem, emitterOffsets) + 6 * sizeof(glm::vec3), .mHidden = true },
        { .mName = "emitterOffset7", .mKind = FieldKind::Vec3, .mOffset = offsetof(ParticleSystem, emitterOffsets) + 7 * sizeof(glm::vec3), .mHidden = true },
        { .mName = "emitterOffsetPlacement0", .mKind = FieldKind::Int, .mOffset = offsetof(ParticleSystem, offsetPlacementIds), .mHidden = true },
        { .mName = "emitterOffsetPlacement1", .mKind = FieldKind::Int, .mOffset = offsetof(ParticleSystem, offsetPlacementIds) + 1 * sizeof(uint32_t), .mHidden = true },
        { .mName = "emitterOffsetPlacement2", .mKind = FieldKind::Int, .mOffset = offsetof(ParticleSystem, offsetPlacementIds) + 2 * sizeof(uint32_t), .mHidden = true },
        { .mName = "emitterOffsetPlacement3", .mKind = FieldKind::Int, .mOffset = offsetof(ParticleSystem, offsetPlacementIds) + 3 * sizeof(uint32_t), .mHidden = true },
        { .mName = "emitterOffsetPlacement4", .mKind = FieldKind::Int, .mOffset = offsetof(ParticleSystem, offsetPlacementIds) + 4 * sizeof(uint32_t), .mHidden = true },
        { .mName = "emitterOffsetPlacement5", .mKind = FieldKind::Int, .mOffset = offsetof(ParticleSystem, offsetPlacementIds) + 5 * sizeof(uint32_t), .mHidden = true },
        { .mName = "emitterOffsetPlacement6", .mKind = FieldKind::Int, .mOffset = offsetof(ParticleSystem, offsetPlacementIds) + 6 * sizeof(uint32_t), .mHidden = true },
        { .mName = "emitterOffsetPlacement7", .mKind = FieldKind::Int, .mOffset = offsetof(ParticleSystem, offsetPlacementIds) + 7 * sizeof(uint32_t), .mHidden = true },
    };
    inline constexpr TransientField kParticleSystemTransient[] = {
        { offsetof(ParticleSystem, instance), sizeof(ParticleSystem::instance) },
        { offsetof(ParticleSystem, initialized), sizeof(ParticleSystem::initialized) },
        { offsetof(ParticleSystem, wantsPlay), sizeof(ParticleSystem::wantsPlay) },
        { offsetof(ParticleSystem, paused), sizeof(ParticleSystem::paused) },
        { offsetof(ParticleSystem, failedEffect), sizeof(ParticleSystem::failedEffect) },
    };
    static_assert(kMaxSystemEmitters == 8, "kParticleSystemFields lists one offset and one placement id field per slot");
    static_assert(sizeof(uint32_t) == sizeof(int32_t), "the placement id slots are saved through Int fields");

    inline constexpr size_t kNoEmitterOffsetSlot = static_cast<size_t>(-1);

    // The slot that holds this placement's offset, or kNoEmitterOffsetSlot.
    inline size_t FindEmitterOffsetSlot(const ParticleSystem &component, uint32_t placementId)
    {
        if (placementId == 0) return kNoEmitterOffsetSlot;
        for (size_t slot = 0; slot < kMaxSystemEmitters; ++slot)
            if (component.offsetPlacementIds[slot] == placementId) return slot;
        return kNoEmitterOffsetSlot;
    }

    inline size_t FindFreeEmitterOffsetSlot(const ParticleSystem &component)
    {
        for (size_t slot = 0; slot < kMaxSystemEmitters; ++slot)
            if (component.offsetPlacementIds[slot] == 0) return slot;
        return kNoEmitterOffsetSlot;
    }
}
