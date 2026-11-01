/**
 * @file ParticleTestWorld.h
 * @author sumin.park
 * @brief Headless ECS fixture for particle lifecycle and scripting checks.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "ParticleTestAssets.h"

#include <assets/AssetResolver.h>
#include <assets/AssetsModule.h>
#include <core/ecs/CoreModule.h>
#include <core/ecs/EngineMode.h>
#include <core/ecs/SystemScheduler.h>
#include <core/ecs/TransformHierarchy.h>
#include <particles/ParticlesModule.h>
#include <particles/ParticleWorld.h>
#include <particles/components/ParticleSystem.h>

#include <cstring>

namespace mir::test
{
    struct ParticleTestWorld
    {
        ParticleTestAssets files;
        World world;
        CommandBuffer commands;
        SystemScheduler scheduler;
        CoreModule core{{world, scheduler}, commands};
        AssetsModule assets{files.Root()};
        AssetResolver resolver{assets, "assets"};
        ParticlesModule module{{world, scheduler}, assets, resolver, 2};
        SystemContext context{world, commands, 1.0f / 60.0f};

        ParticleTestWorld()
        {
            SetEngineMode(world, EngineMode::Play);
            scheduler.Start(context);
        }
        ~ParticleTestWorld() { scheduler.Stop(context); }
        ParticleWorld &Particles() { return module.Particles(); }
        Entity System(bool autoStart = true)
        {
            const Entity owner = world.CreateEntity();
            AddTransform(world, owner);
            ParticleSystem system;
            std::strcpy(system.systemName, "effects/exhaust.particle.json");
            system.playOnStart = autoStart;
            world.AddComponent<ParticleSystem>(owner, system);
            return owner;
        }
        void Tick(float dt = 1.0f / 60.0f)
        {
            context.dt = dt;
            scheduler.Update(context, 0, 1.0f / 60.0f);
            ++context.frame;
        }
        void StageEvent(Stage stage) { scheduler.RunStage(stage, context); }
    };
}
