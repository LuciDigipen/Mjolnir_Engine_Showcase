/**
 * @file ParticleRibbon.cpp
 * @author sumin.park
 * @brief Distance or maximum interval emission, wind drift and ribbon retirement.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "particles/ParticleRibbon.h"
#include "particles/ParticleRuntime.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>

namespace mir
{
    namespace
    {
        void Appearance(ParticleRibbonPoint &point, const ParticleEmitterAsset &definition, float scale, float age)
        {
            const float t = std::clamp(age, 0.0f, 1.0f);
            const auto &width = definition.updateModules[2];
            point.width = definition.size * scale * (width.name == "RibbonWidthCurve" ?
                EvaluateParticleCurve(width.parameters[0], width.parameters[1], width.parameters[2], t) :
                std::lerp(1.0f, width.parameters[0], t));
            point.color = definition.color;
            if (definition.updateModules.size() == 5)
            {
                const auto &p = definition.updateModules[3].parameters;
                point.color *= glm::vec4(std::lerp(p[0], p[4], t), std::lerp(p[1], p[5], t),
                    std::lerp(p[2], p[6], t), std::lerp(p[3], p[7], t));
            }
            point.color.a *= 1 - t;
        }
    }

    void AdvanceParticleRibbon(ParticleEmitterInstance &emitter, const ParticleUserParameters &parameters,
        uint32_t &budget, ParticleRuntimeStats &stats)
    {
        auto &state = emitter.ribbon;
        const auto &definition = emitter.definition;
        const double dt = emitter.seconds;
        if (dt <= 0) return;
        state.time += dt;
        const auto drift = WorldVec3(parameters.wind) * WorldReal(dt);
        for (auto &point : state.points)
        {
            const auto moved = WorldPosition::FromMeters(point.position.Meters() + drift);
            bool supported = moved.has_value();
            if constexpr (sizeof(WorldReal) == sizeof(float))
                if (moved)
                    for (int axis = 0; axis < 3; ++axis)
                        supported &= std::abs(moved->Meters()[axis]) <= kParticleFloatWorldLimitMeters;
            if (supported) point.position = *moved;
            else { point.birthTime = -definition.lifetime; ++stats.rejectedRibbonPoints; }
            const float age = static_cast<float>((state.time - point.birthTime) / definition.lifetime);
            Appearance(point, definition, parameters.sizeScale, age);
        }
        std::erase_if(state.points, [&](const auto &point) { return state.time - point.birthTime >= definition.lifetime; });
        if (emitter.state != ParticleExecutionState::Active || parameters.spawnRateScale == 0)
        {
            state.connected = false;
            return;
        }
        if (emitter.teleported) state.connected = false;
        const auto delta = emitter.attachmentPosition.Meters() - emitter.spawnStart.Meters();
        const double length = static_cast<double>(glm::length(delta));
        const double spacing = definition.spawnPerMeter * parameters.spawnRateScale > 0 ?
            1.0 / (definition.spawnPerMeter * parameters.spawnRateScale) : 1e30;
        const double interval = definition.spawnModules[0].parameters[0] / std::max(0.0001f, parameters.spawnRateScale);
        auto birth = [&](double fraction)
        {
            if (!budget)
            {
                ++stats.droppedBudgetSpawns;
                state.connected = false;
                return;
            }
            --budget;
            const double age = dt * (1 - fraction);
            if (age >= definition.lifetime) return;
            const auto position = WorldPosition::FromMeters(emitter.spawnStart.Meters() + delta * WorldReal(fraction) +
                WorldVec3(parameters.wind) * WorldReal(age));
            bool supported = position.has_value();
            if constexpr (sizeof(WorldReal) == sizeof(float))
                if (position)
                    for (int axis = 0; axis < 3; ++axis)
                        supported &= std::abs(position->Meters()[axis]) <= kParticleFloatWorldLimitMeters;
            if (!supported)
            {
                ++stats.rejectedRibbonPoints;
                state.connected = false;
                return;
            }
            if (!state.connected)
            {
                ++state.ribbon;
                state.textureDistance = 0;
                state.connected = true;
            }
            if (state.points.size() == definition.capacity)
            {
                state.points.erase(state.points.begin());
                ++stats.evictedRibbonPoints;
            }
            ParticleRibbonPoint point;
            point.position = *position;
            point.birthTime = state.time - age;
            point.textureDistance = state.textureDistance;
            point.ribbon = state.ribbon;
            point.link = state.nextLink++;
            const float normalized = static_cast<float>(age / definition.lifetime);
            Appearance(point, definition, parameters.sizeScale, normalized);
            state.points.push_back(point);
            ++emitter.spawns;
        };
        if (!state.connected)
        {
            state.distance = state.sinceBirth = 0;
            birth(0);
        }
        double fraction = 0;
        // At most the shared birth budget plus one rejected interval, excess work never queues.
        while (fraction < 1)
        {
            const double byDistance = length > 0 ? (spacing - state.distance) / length : 1e30;
            const double byTime = (interval - state.sinceBirth) / dt;
            const double step = std::max(1e-10, std::min({byDistance, byTime, 1 - fraction}));
            state.distance += length * step;
            state.textureDistance += length * step;
            state.sinceBirth += dt * step;
            fraction += step;
            if (state.distance + 1e-8 >= spacing || state.sinceBirth + 1e-8 >= interval)
            {
                birth(std::min(fraction, 1.0));
                state.distance = state.sinceBirth = 0;
                if (!budget)
                {
                    const double repeat = std::min(length > 0 ? spacing / length : 1e30, interval / dt);
                    stats.droppedBudgetSpawns += static_cast<uint64_t>(std::floor((1 - fraction) / repeat + 1e-8));
                    state.connected = false;
                    break;
                }
            }
        }
    }
}
