/**
 * @file PhysicsInterpolation.tests.cpp
 * @author Quy Dong Ta
 * @brief Tests for physics/PhysicsInterpolationSystem: the step-pose blend and
 *        its hand-off with PhysicsSystem's teleport detection.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <physics/PhysicsInterpolationSystem.h>
#include <physics/PhysicsSystem.h>

#include <physics/PhysicsWorld.h>
#include <physics/components/RigidBody.h>

#include <core/ecs/CommandBuffer.h>
#include <core/ecs/EngineMode.h>
#include <core/ecs/SystemScheduler.h>
#include <core/ecs/TransformHierarchy.h>
#include <core/ecs/World.h>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <optional>

namespace
{
    constexpr float kStep = 1.0f / 60.0f;

    // PhysicsSystem and the blend after it, registered as App::Initialize does.
    struct InterpolationFixture
    {
        mir::World world;
        mir::CommandBuffer commands;
        mir::SystemScheduler scheduler;
        mir::SystemContext context{ world, commands, kStep, 0.0, 0 };

        InterpolationFixture()
        {
            world.EmplaceResource<mir::PhysicsWorld>();
            world.EmplaceResource<mir::EngineModeState>();
            mir::SetEngineMode(world, mir::EngineMode::Play);
            mir::SetPlayState(world, mir::PlayState::Running);

            scheduler.AddSystem<mir::PhysicsSystem>(mir::SystemPhase::FixedUpdate, mir::SystemOrder{mir::SystemOrder::kNormal});
            scheduler.AddSystem<mir::PhysicsInterpolationSystem>(mir::SystemPhase::PostUpdate, mir::SystemOrder{mir::SystemOrder::kNormal});
            scheduler.Start(context);
        }

        ~InterpolationFixture()
        {
            if (scheduler.Started())
                scheduler.Stop(context);
        }

        // One frame of `steps` substeps, drawn `alpha` of the way into the next.
        void Frame(uint32_t steps, float alpha)
        {
            context.fixedAlpha = alpha;
            scheduler.Update(context, steps, kStep);
        }

        mir::Entity Add(const mir::WorldVec3 &position, bool interpolate)
        {
            const mir::Entity entity = world.CreateEntity();
            mir::AddTransform(world, entity, mir::Transform{ position });
            mir::RigidBody rigidBody{
                .motion = mir::PhysicsMotion::Dynamic,
                .shape = mir::PhysicsShape::Sphere,
                .radius = 0.5f,
                .interpolate = interpolate,
            };
            world.AddComponent<mir::RigidBody>(entity, rigidBody);
            return entity;
        }

        mir::Transform &TransformOf(mir::Entity entity) { return *world.GetComponent<mir::Transform>(entity); }
        const mir::RigidBody &BodyOf(mir::Entity entity) { return *world.GetComponent<mir::RigidBody>(entity); }

        mir::WorldVec3 SimulatedMeters(mir::Entity entity)
        {
            const std::optional<mir::WorldPosition> position =
                world.Resource<mir::PhysicsWorld>().BodyPosition(BodyOf(entity).runtime.body);
            REQUIRE(position);
            return position->Meters();
        }
    };
}

TEST_CASE("An interpolating body is drawn between its last two steps", "[physics][interpolation]")
{
    InterpolationFixture fixture;
    const mir::Entity entity = fixture.Add(mir::WorldVec3(0.0, 5.0, 0.0), true);

    for (int i = 0; i < 10; ++i)
        fixture.Frame(1, 1.0f);

    const mir::RigidBody &body = fixture.BodyOf(entity);
    const double previousY = body.runtime.previousPosition.y;
    const double currentY = body.runtime.currentPosition.y;
    REQUIRE(previousY > currentY); // falling

    // A frame between steps: half way from the last pose to the newest.
    fixture.Frame(0, 0.5f);
    const double drawnY = fixture.TransformOf(entity).Position().y;
    REQUIRE(std::abs(drawnY - (previousY + currentY) * 0.5) < 1e-5);

    // Alpha 1 is the newest step exactly.
    fixture.Frame(0, 1.0f);
    REQUIRE(std::abs(fixture.TransformOf(entity).Position().y - currentY) < 1e-5);
}

TEST_CASE("A body without interpolate still shows its latest step", "[physics][interpolation]")
{
    InterpolationFixture fixture;
    const mir::Entity entity = fixture.Add(mir::WorldVec3(0.0, 5.0, 0.0), false);

    for (int i = 0; i < 10; ++i)
        fixture.Frame(1, 1.0f);
    fixture.Frame(0, 0.25f);

    REQUIRE(fixture.TransformOf(entity).Position() == fixture.BodyOf(entity).runtime.currentPosition);
}

TEST_CASE("Interpolated writes are not read back as a teleport", "[physics][interpolation]")
{
    InterpolationFixture fixture;
    const mir::Entity smooth = fixture.Add(mir::WorldVec3(-5.0, 5.0, 0.0), true);
    const mir::Entity reference = fixture.Add(mir::WorldVec3(5.0, 5.0, 0.0), false);

    // Uneven frames, as a display out of step with physics produces.
    for (int i = 0; i < 20; ++i)
        fixture.Frame(i % 3 == 0 ? 0u : 1u, (i % 4) * 0.25f);

    // The simulation itself never saw the blend: both bodies fell the same.
    const mir::WorldVec3 smoothBody = fixture.SimulatedMeters(smooth);
    const mir::WorldVec3 referenceBody = fixture.SimulatedMeters(reference);
    REQUIRE(std::abs(smoothBody.y - referenceBody.y) < 1e-5);
    REQUIRE(smoothBody.y < 5.0);
}

TEST_CASE("An outside Transform write between steps wins over the blend", "[physics][interpolation]")
{
    InterpolationFixture fixture;
    const mir::Entity entity = fixture.Add(mir::WorldVec3(0.0, 5.0, 0.0), true);

    for (int i = 0; i < 10; ++i)
        fixture.Frame(1, 1.0f);

    // What a script that assigns Transform.position looks like from here.
    fixture.TransformOf(entity).SetPosition(mir::WorldVec3(3.0, 10.0, 0.0));
    fixture.Frame(0, 0.5f);
    REQUIRE(fixture.TransformOf(entity).Position() == mir::WorldVec3(3.0, 10.0, 0.0));

    // The next step teleports the body, and the blend restarts from there.
    fixture.Frame(1, 0.0f);
    const mir::WorldVec3 drawn = fixture.TransformOf(entity).Position();
    REQUIRE(std::abs(drawn.x - 3.0) < 1e-5);
    REQUIRE(std::abs(drawn.y - 10.0) < 1e-5);
    REQUIRE(fixture.SimulatedMeters(entity).y < 10.0);
}
