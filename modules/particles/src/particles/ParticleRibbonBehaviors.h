/**
 * @file ParticleRibbonBehaviors.h
 * @author sumin.park
 * @brief Attributes a CPU ribbon stack assigns, shared by the registry and the ribbon simulation.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "particles/ParticleEffect.h"

#include <glm/vec4.hpp>

namespace mir
{
    // Each behavior assigns the attributes it owns, so a later entry overrides an earlier one.
    struct ParticleRibbonLook
    {
        float size = 0;
        bool curve = false;
        float start = 1, middle = 1, end = 1;
        glm::vec4 base{1}, color{1}, delta{0}; // base is the emitter color a gradient scales
        bool fade = false;
        bool wind = false;
        float interval = 0.25f;
    };

    void ResolveRibbonSpawn(ParticleRibbonLook &look, const ParticleBehavior &behavior);
    void ResolveRibbonWind(ParticleRibbonLook &look, const ParticleBehavior &behavior);
    void ResolveRibbonWidth(ParticleRibbonLook &look, const ParticleBehavior &behavior);
    void ResolveRibbonWidthCurve(ParticleRibbonLook &look, const ParticleBehavior &behavior);
    void ResolveRibbonColorGradient(ParticleRibbonLook &look, const ParticleBehavior &behavior);
    void ResolveRibbonFade(ParticleRibbonLook &look, const ParticleBehavior &behavior);
    void ResolveRibbonAge(ParticleRibbonLook &look, const ParticleBehavior &behavior);
}
