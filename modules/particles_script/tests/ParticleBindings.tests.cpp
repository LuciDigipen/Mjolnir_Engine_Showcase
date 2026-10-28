/**
 * @file ParticleBindings.tests.cpp
 * @author sumin.park
 * @brief Exercises Lua requests against the real headless playback resource.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "ParticleTestWorld.h"

#include <particles_script/ParticleBindings.h>
#include <script/BindingApi.h>
#include <script/ScriptHost.h>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("Particle Lua controls drive CPU playback without a renderer", "[particles_script][particles]")
{
    mir::test::ParticleTestWorld test;
    const auto owner = test.Emitter(false);
    mir::ScriptHost host;
    const auto bindings = mir::AddBindings(host, &mir::RegisterParticleBindings);
    REQUIRE(host.LoadScriptSource("particle_controls", R"(
        local Controls = {}
        function Controls.OnStart(self, world, entity)
            assert(world:particle_state(entity) == "stopped")
            assert(world:particle_start(entity))
        end
        function Controls.OnUpdate(self, world, entity, dt)
            assert(world:particle_state(entity) == "emitting")
            assert(world:particle_pause(entity, true))
            assert(world:particle_state(entity) == "paused")
            assert(world:particle_pause(entity, false))
            assert(world:particle_stop(entity))
            assert(world:particle_state(entity) == "draining")
            assert(world:particle_reset(entity))
            assert(world:particle_state(entity) == "stopped")
            assert(world:particle_destroy(entity))
        end
        return Controls
    )"));
    const auto instance = host.CreateInstance("particle_controls");
    REQUIRE(instance != -1);
    host.CallOnStart(test.world, test.commands, owner, instance);
    test.Tick();
    REQUIRE(test.Particles().Find(owner));
    host.CallOnUpdate(test.world, test.commands, owner, instance, 1.0f / 60);
    CHECK_FALSE(test.Particles().Find(owner));
    test.Tick();
    CHECK(test.Particles().Runtime().Stats().active == 0);
    host.DestroyInstance(instance);
}
