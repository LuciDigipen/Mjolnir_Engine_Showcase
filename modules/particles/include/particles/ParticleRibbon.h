/**
 * @file ParticleRibbon.h
 * @author sumin.park
 * @brief World precision CPU ribbon history and bounded simulation.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/math/WorldCoordinates.h>

#include <glm/vec4.hpp>

#include <cstdint>
#include <vector>

namespace mir
{
    struct ParticleEmitterInstance;
    struct ParticleUserParameters;
    struct ParticleRuntimeStats;
    struct ParticleRibbonPoint
    {
        WorldPosition position;
        double birthTime = 0;
        double textureDistance = 0;
        uint64_t ribbon = 0;
        uint64_t link = 0;
        float width = 0;
        glm::vec4 color{0};
    };
    struct ParticleRibbonState
    {
        std::vector<ParticleRibbonPoint> points;
        double time = 0;
        double sinceBirth = 0;
        double distance = 0;
        double textureDistance = 0;
        uint64_t ribbon = 0;
        uint64_t nextLink = 0;
        bool connected = false;
    };
    void AdvanceParticleRibbon(ParticleEmitterInstance &emitter, const ParticleUserParameters &parameters,
        uint32_t &budget, ParticleRuntimeStats &stats);
}
