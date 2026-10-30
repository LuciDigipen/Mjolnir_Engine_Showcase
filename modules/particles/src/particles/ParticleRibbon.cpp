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
#include "ParticleRibbonBehaviors.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>

namespace mir
{
    void ResolveRibbonSpawn(ParticleRibbonLook &look, const ParticleBehavior &behavior) { look.interval = behavior.parameters[0]; }
    void ResolveRibbonWind(ParticleRibbonLook &look, const ParticleBehavior &) { look.wind = true; }
    // Retirement at the end of the lifetime is always on, the behavior only has to be present.
    void ResolveRibbonAge(ParticleRibbonLook &, const ParticleBehavior &) {}
    void ResolveRibbonFade(ParticleRibbonLook &look, const ParticleBehavior &) { look.fade = true; }
    void ResolveRibbonWidth(ParticleRibbonLook &look, const ParticleBehavior &behavior)
    {
        look.curve = false;
        look.start = look.middle = 1;
        look.end = behavior.parameters[0];
    }
    void ResolveRibbonWidthCurve(ParticleRibbonLook &look, const ParticleBehavior &behavior)
    {
        look.curve = true;
        look.start = behavior.parameters[0];
        look.middle = behavior.parameters[1];
        look.end = behavior.parameters[2];
    }
    // The gradient assigns the whole color including alpha, so it also drops an earlier fade.
    void ResolveRibbonColorGradient(ParticleRibbonLook &look, const ParticleBehavior &behavior)
    {
        const auto &p = behavior.parameters;
        const glm::vec4 first(p[0], p[1], p[2], p[3]), last(p[4], p[5], p[6], p[7]);
        look.color = look.base * first;
        look.delta = look.base * (last - first);
        look.fade = false;
    }

    namespace
    {
        ParticleRibbonLook ResolveLook(ParticleRibbonState &state, const ParticleEmitterAsset &definition, float scale)
        {
            if (state.resolvers.empty())
            {
                for (const auto *stack : { &definition.spawnBehaviors, &definition.updateBehaviors })
                    for (const auto &behavior : *stack)
                    {
                        const auto found = std::find_if(ParticleBehaviors().begin(), ParticleBehaviors().end(),
                            [&](const ParticleBehaviorInfo &entry) { return entry.name == behavior.name; });
                        state.resolvers.push_back(found->resolve);
                    }
            }
            ParticleRibbonLook look;
            look.size = definition.size * scale;
            look.base = look.color = definition.color;
            size_t next = 0;
            for (const auto *stack : { &definition.spawnBehaviors, &definition.updateBehaviors })
                for (const auto &behavior : *stack)
                    state.resolvers[next++](look, behavior);
            return look;
        }

        void Apply(const ParticleRibbonLook &look, ParticleRibbonPoint &point, float age)
        {
            const float t = std::clamp(age, 0.0f, 1.0f);
            point.width = look.size * (look.curve ? EvaluateParticleCurve(look.start, look.middle, look.end, t) :
                std::lerp(look.start, look.end, t));
            point.color = look.color + look.delta * t;
            if (look.fade) point.color.a *= 1 - t;
        }
    }

    void AdvanceParticleRibbon(ParticleEmitterInstance &emitter, const ParticleUserParameters &parameters,
        uint32_t &budget, ParticleRuntimeStats &stats)
    {
        auto &state = emitter.ribbon;
        const auto &definition = emitter.definition;
        const double dt = emitter.seconds;
        if (dt <= 0) return;
        const auto look = ResolveLook(state, definition, parameters.sizeScale);
        state.time += dt;
        const auto wind = look.wind ? WorldVec3(parameters.wind) : WorldVec3(0);
        const auto drift = wind * WorldReal(dt);
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
            Apply(look, point, age);
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
        const double interval = look.interval / std::max(0.0001f, parameters.spawnRateScale);
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
                wind * WorldReal(age));
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
            Apply(look, point, normalized);
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
