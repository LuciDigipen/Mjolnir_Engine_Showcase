/**
 * @file ParticleBindings.tests.cpp
 * @author sumin.park
 * @brief Exercises Lua requests against the real headless playback resource.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "ParticleTestWorld.h"

#include <core/ecs/ComponentRegistry.h>
#include <particles/components/ParticleSystem.h>
#include <particles_script/ParticleBindings.h>
#include <script/BindingApi.h>
#include <script/ScriptHost.h>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("Particle Lua controls drive CPU playback without a renderer", "[particles_script][particles]")
{
    mir::test::ParticleTestWorld test;
    const auto owner = test.System(false);
    mir::ScriptHost host;
    const auto bindings = mir::AddBindings(host, &mir::RegisterParticleBindings);
    REQUIRE(host.LoadScriptSource("particle_controls", R"(
        local Controls = {}
        function Controls.OnStart(self, world, entity)
            assert(world:particle_system_state(entity) == "complete")
            assert(world:particle_system_parameters(entity, 2, 0.5))
            assert(not world:particle_system_parameters(entity, -1, 1))
            assert(world:particle_system_start(entity))
        end
        function Controls.OnUpdate(self, world, entity, dt)
            assert(world:particle_system_state(entity) == "active")
            assert(world:particle_system_pause(entity, true))
            assert(world:particle_system_state(entity) == "paused")
            assert(world:particle_system_pause(entity, false))
            assert(world:particle_system_stop(entity))
            assert(world:particle_system_state(entity) == "inactive")
            assert(world:particle_system_reset(entity))
            assert(world:particle_system_state(entity) == "complete")
            assert(world:particle_system_destroy(entity))
        end
        return Controls
    )"));
    const auto instance = host.CreateInstance("particle_controls");
    REQUIRE(instance != -1);
    host.CallOnStart(test.world, test.commands, owner, instance);
    test.Tick();
    const auto *system = test.Particles().Find(owner);
    REQUIRE(system);
    CHECK(system->parameters.spawnRateScale == 2);
    CHECK(system->parameters.sizeScale == 0.5f);
    const auto *component = test.world.GetComponent<mir::ParticleSystem>(owner);
    REQUIRE(component);
    CHECK(component->overrideParameters);
    CHECK(component->userParameters.spawnRateScale == 2);
    CHECK(component->userParameters.sizeScale == 0.5f);
    const auto *ops = mir::ComponentRegistry::Instance().Find("ParticleSystem");
    REQUIRE(ops);
    CHECK(ops->FindField("spawnRateScale"));
    CHECK(ops->FindField("sizeScale"));
    CHECK(mir::ComponentRegistry::Instance().Find("ParticleEmitter") == nullptr);
    host.CallOnUpdate(test.world, test.commands, owner, instance, 1.0f / 60);
    CHECK_FALSE(test.Particles().Find(owner));
    test.Tick();
    CHECK(test.Particles().Runtime().Stats().active == 0);
    host.DestroyInstance(instance);
}

TEST_CASE("Particle Lua user overrides update live systems and reject invalid replacements", "[particles_script][particles]")
{
    mir::test::ParticleTestWorld test;
    const auto owner = test.System(false);
    mir::ScriptHost host;
    const auto bindings = mir::AddBindings(host, &mir::RegisterParticleBindings);
    REQUIRE(host.LoadScriptSource("particle_parameters", R"(
        local Controls = {}
        function Controls.OnStart(self, world, entity)
            assert(world:particle_system_start(entity))
        end
        function Controls.OnUpdate(self, world, entity, dt)
            assert(world:particle_system_parameters(entity, 1.5, 2))
            assert(world:particle_system_wind(entity, 3, 4, -2))
            assert(not world:particle_system_wind(entity, 0/0, 0, 0))
            assert(not world:particle_system_parameters(entity, 17, 1))
            assert(not world:particle_system_parameters(entity, 1, 0/0))
        end
        return Controls
    )"));
    const auto instance = host.CreateInstance("particle_parameters");
    REQUIRE(instance != -1);
    host.CallOnStart(test.world, test.commands, owner, instance);
    test.Tick();
    REQUIRE(test.Particles().Find(owner));
    host.CallOnUpdate(test.world, test.commands, owner, instance, 1.0f / 60);
    const auto *system = test.Particles().Find(owner);
    REQUIRE(system);
    CHECK(system->parameters.spawnRateScale == 1.5f);
    CHECK(system->parameters.sizeScale == 2);
    CHECK(system->parameters.wind == glm::vec3(3, 4, -2));
    const auto *component = test.world.GetComponent<mir::ParticleSystem>(owner);
    REQUIRE(component);
    CHECK(component->userParameters.spawnRateScale == 1.5f);
    CHECK(component->userParameters.sizeScale == 2);
    CHECK(component->userParameters.wind == glm::vec3(3, 4, -2));
    host.DestroyInstance(instance);
}

TEST_CASE("Particle Lua setters before playback keep the other authored defaults", "[particles_script][particles]")
{
    mir::test::ParticleTestWorld test;
    test.files.authoredParameters = { 0.5f, 2, glm::vec3(1, 2, 3) };
    test.files.Publish();
    const auto owner = test.System(false);
    mir::ScriptHost host;
    const auto bindings = mir::AddBindings(host, &mir::RegisterParticleBindings);
    REQUIRE(host.LoadScriptSource("particle_preplay", R"(
        local Controls = {}
        function Controls.OnUpdate(self, world, entity, dt)
            assert(world:particle_system_parameters(entity, 3, 4))
        end
        return Controls
    )"));
    const auto instance = host.CreateInstance("particle_preplay");
    REQUIRE(instance != -1);
    host.CallOnUpdate(test.world, test.commands, owner, instance, 1.0f / 60);
    const auto *component = test.world.GetComponent<mir::ParticleSystem>(owner);
    REQUIRE(component);
    CHECK(component->userParameters.spawnRateScale == 3);
    CHECK(component->userParameters.sizeScale == 4);
    CHECK(component->userParameters.wind == glm::vec3(1, 2, 3));
    host.DestroyInstance(instance);
}
