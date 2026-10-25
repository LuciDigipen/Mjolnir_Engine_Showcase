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

#include <physics/PhysicsStepGate.h>
#include <physics/PhysicsWorld.h>
#include <physics/components/RigidBody.h>

#include <core/ecs/CommandBuffer.h>
#include <core/ecs/EngineMode.h>
#include <core/ecs/SystemScheduler.h>
#include <core/ecs/TransformHierarchy.h>
#include <core/ecs/World.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>

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
        // what PhysicsModule sets, so an added RigidBody gets its body
        mir::Registration hooks;

        InterpolationFixture()
        {
            world.EmplaceResource<mir::PhysicsWorld>();
            world.Resource<mir::PhysicsWorld>().SetSimulationLive(true);
            hooks = world.SetComponentHooks<mir::RigidBody>(
                { .onAdd = &mir::PhysicsSystem::OnRigidBodyAdded, .onRemove = &mir::PhysicsSystem::OnRigidBodyRemoved });
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

namespace
{
    // Reads what the body is at, after the push and before the step, which is where a write lands.
    class BodyProbe final : public mir::PhysicsStepGate
    {
    public:
        explicit BodyProbe(mir::World &world) : mWorld(world)
        {
            mWorld.EmplaceResource<mir::PhysicsStepGates>().mGates.push_back(this);
        }

        ~BodyProbe() override { mWorld.Resource<mir::PhysicsStepGates>().mGates.clear(); }

        void Watch(mir::BodyHandle body) { mBody = body; }

        void BeforeStep(mir::World &, mir::PhysicsWorld &physics, float) override
        {
            position = physics.BodyPosition(mBody).value().Meters();
            rotation = physics.BodyRotation(mBody);
            linear = physics.BodyLinearVelocity(mBody);
            angular = physics.BodyAngularVelocity(mBody);
        }

        mir::WorldVec3 position{ 0.0 };
        glm::quat rotation{ 1.0f, 0.0f, 0.0f, 0.0f };
        glm::vec3 linear{ 0.0f };
        glm::vec3 angular{ 0.0f };

    private:
        mir::World &mWorld;
        mir::BodyHandle mBody;
    };

    // A body in flight with both position and rotation changing, drawn half way between its steps so the
    // entity lags the body.
    struct InFlight
    {
        InterpolationFixture fixture;
        BodyProbe probe{ fixture.world };
        mir::Entity entity;
        mir::WorldVec3 bodyPosition{ 0.0 };
        glm::quat bodyRotation{ 1.0f, 0.0f, 0.0f, 0.0f };
        glm::vec3 linear{ 0.0f };
        glm::vec3 angular{ 0.0f };

        explicit InFlight(bool interpolate = true)
        {
            entity = fixture.Add(mir::WorldVec3(0.0, 500.0, 0.0), interpolate);
            const mir::BodyHandle body = fixture.BodyOf(entity).runtime.body;
            probe.Watch(body);
            fixture.world.Resource<mir::PhysicsWorld>().SetBodyLinearVelocity(body, glm::vec3(4.0f, 0.0f, 0.0f));
            fixture.world.Resource<mir::PhysicsWorld>().SetBodyAngularVelocity(body, glm::vec3(0.0f, 0.0f, 3.0f));
            for (int i = 0; i < 5; ++i)
                fixture.Frame(1, 0.5f);
            Record();
        }

        // what the body is at right now, before the write under test
        void Record()
        {
            mir::PhysicsWorld &physics = fixture.world.Resource<mir::PhysicsWorld>();
            const mir::BodyHandle body = fixture.BodyOf(entity).runtime.body;
            bodyPosition = physics.BodyPosition(body).value().Meters();
            bodyRotation = physics.BodyRotation(body);
            linear = physics.BodyLinearVelocity(body);
            angular = physics.BodyAngularVelocity(body);
        }
    };

    bool Same(const glm::quat &a, const glm::quat &b) { return std::abs(glm::dot(a, b)) > 0.999999f; }
}

TEST_CASE("Writing the position of an interpolated body moves it there and keeps its rotation", "[physics][interpolation]")
{
    InFlight f;
    // the entity lags the body, so a write that reads every field back would pull the body to the lagging pose
    REQUIRE(f.fixture.TransformOf(f.entity).Position() != f.bodyPosition);

    const mir::WorldVec3 target(100.0, 500.0, 0.0);
    f.fixture.TransformOf(f.entity).SetPosition(target);
    f.fixture.Frame(1, 0.5f);

    CHECK(f.probe.position == target);
    CHECK(Same(f.probe.rotation, f.bodyRotation));
    CHECK(f.probe.linear == f.linear);
    CHECK(f.probe.angular == f.angular);
}

TEST_CASE("Writing the rotation of an interpolated body turns it and keeps its position", "[physics][interpolation]")
{
    InFlight f;
    REQUIRE(!Same(f.fixture.TransformOf(f.entity).Rotation(), f.bodyRotation));

    const glm::quat target = glm::angleAxis(1.0f, glm::vec3(0.0f, 1.0f, 0.0f));
    f.fixture.TransformOf(f.entity).SetRotation(target);
    f.fixture.Frame(1, 0.5f);

    CHECK(Same(f.probe.rotation, target));
    CHECK(f.probe.position == f.bodyPosition);
    CHECK(f.probe.linear == f.linear);
}

TEST_CASE("Writing the scale of an interpolated body leaves its pose alone", "[physics][interpolation]")
{
    InFlight f;
    f.fixture.TransformOf(f.entity).SetScale(glm::vec3(2.0f));
    f.fixture.Frame(1, 0.5f);

    CHECK(f.probe.position == f.bodyPosition);
    CHECK(Same(f.probe.rotation, f.bodyRotation));

    // and the next drawn frame does not jump: alpha 1 is the newest step, as without the write
    f.fixture.Frame(0, 1.0f);
    CHECK(f.fixture.TransformOf(f.entity).Position() == f.fixture.BodyOf(f.entity).runtime.currentPosition);
}

TEST_CASE("After a position write interpolation restarts from the new pose", "[physics][interpolation]")
{
    InFlight f;
    const mir::WorldVec3 target(100.0, 500.0, 0.0);
    f.fixture.TransformOf(f.entity).SetPosition(target);

    // alpha 0 draws the pose the step started from: the written position, not the old one
    f.fixture.Frame(1, 0.0f);
    CHECK(f.fixture.TransformOf(f.entity).Position().x == Catch::Approx(100.0).margin(1e-9));
    CHECK(f.fixture.TransformOf(f.entity).Position().y == Catch::Approx(500.0).margin(1e-9));

    // alpha 1 draws the stepped pose, one step of the 4 m/s on from the written position
    f.fixture.Frame(0, 1.0f);
    CHECK(f.fixture.TransformOf(f.entity).Position().x == Catch::Approx(100.0 + 4.0 / 60.0).margin(1e-3));
}

TEST_CASE("A position write on a body that is not interpolated keeps its rotation", "[physics][interpolation]")
{
    InFlight f(false);
    const mir::WorldVec3 target(50.0, 500.0, 0.0);
    f.fixture.TransformOf(f.entity).SetPosition(target);
    f.fixture.Frame(1, 0.5f);

    CHECK(f.probe.position == target);
    CHECK(Same(f.probe.rotation, f.bodyRotation));
}

TEST_CASE("A local write on a parented interpolated body is read through its parent", "[physics][interpolation]")
{
    InterpolationFixture fixture;
    BodyProbe probe(fixture.world);

    const mir::Entity parent = fixture.world.CreateEntity();
    mir::AddTransform(fixture.world, parent, mir::Transform{ mir::WorldVec3(10.0, 0.0, 0.0) });
    const mir::Entity child = fixture.world.CreateEntity();
    mir::AddTransform(fixture.world, child, mir::Transform{ mir::WorldVec3(0.0, 500.0, 0.0) }, parent);
    fixture.world.AddComponent<mir::RigidBody>(child,
        mir::RigidBody{ .motion = mir::PhysicsMotion::Dynamic, .shape = mir::PhysicsShape::Sphere, .radius = 0.5f, .interpolate = true });
    probe.Watch(fixture.BodyOf(child).runtime.body);
    for (int i = 0; i < 5; ++i)
        fixture.Frame(1, 0.5f);

    fixture.TransformOf(child).SetPosition(mir::WorldVec3(3.0, 500.0, 0.0));
    fixture.Frame(1, 0.5f);

    // local 3 under a parent at 10 is world 13
    CHECK(probe.position.x == Catch::Approx(13.0).margin(1e-9));
}
