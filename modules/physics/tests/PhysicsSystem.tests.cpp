/**
 * @file PhysicsSystem.tests.cpp
 * @author Jackey Cen
 * @brief Tests for physics/PhysicsSystem: body lifetime and Transform sync.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <physics/PhysicsSystem.h>

#include <physics/CollisionEvents.h>
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

#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <optional>
#include <vector>

namespace
{
    constexpr float kStep = 1.0f / 60.0f;

    // The slice of App::Initialize a physics system actually needs.
    struct PhysicsFixture
    {
        mir::World world;
        mir::CommandBuffer commands;
        mir::SystemScheduler scheduler;
        mir::SystemContext context{ world, commands, kStep, 0.0, 0 };
        // what PhysicsModule sets, so an added RigidBody gets its body and a removed one frees it
        mir::Registration hooks;

        explicit PhysicsFixture(mir::EngineMode mode = mir::EngineMode::Play)
        {
            world.EmplaceResource<mir::PhysicsWorld>();
            hooks = world.SetComponentHooks<mir::RigidBody>(
                { .onAdd = &mir::PhysicsSystem::OnRigidBodyAdded, .onRemove = &mir::PhysicsSystem::OnRigidBodyRemoved });
            world.EmplaceResource<mir::EngineModeState>();
            mir::SetEngineMode(world, mode);
            mir::SetPlayState(world, mir::PlayState::Running);
            Physics().SetSimulationLive(mode == mir::EngineMode::Play);

            scheduler.AddSystem<mir::PhysicsSystem>(mir::SystemPhase::FixedUpdate, mir::SystemOrder{mir::SystemOrder::kNormal});
            scheduler.Start(context);
        }

        ~PhysicsFixture()
        {
            if (scheduler.Started())
                scheduler.Stop(context);
        }

        // One frame with exactly one fixed substep, as App::Run would drive it.
        void Run(int steps)
        {
            for (int i = 0; i < steps; ++i)
                scheduler.Update(context, 1, kStep);
        }

        mir::PhysicsWorld &Physics() { return world.Resource<mir::PhysicsWorld>(); }

        mir::Entity Add(const mir::WorldVec3 &position, const mir::RigidBody &rigidBody)
        {
            const mir::Entity entity = world.CreateEntity();
            mir::AddTransform(world, entity, mir::Transform{ position });
            world.AddComponent<mir::RigidBody>(entity, rigidBody);
            return entity;
        }

        const mir::Transform &TransformOf(mir::Entity entity)
        {
            return *world.GetComponent<mir::Transform>(entity);
        }
    };

    constexpr mir::RigidBody kDynamicSphere{
        .motion = mir::PhysicsMotion::Dynamic,
        .shape = mir::PhysicsShape::Sphere,
        .radius = 0.5f,
    };

    constexpr mir::RigidBody kStaticBox{
        .motion = mir::PhysicsMotion::Static,
        .shape = mir::PhysicsShape::Box,
        .halfExtent = glm::vec3(10.0f, 0.5f, 10.0f),
    };
}

TEST_CASE("A dynamic RigidBody drives its Transform", "[physics]")
{
    PhysicsFixture fixture;
    const mir::Entity entity = fixture.Add(glm::vec3(0.0f, 5.0f, 0.0f), kDynamicSphere);

    fixture.Run(30);

    // Half a second of free fall is ~1.2m; a loose bound keeps this off how
    // Jolt spreads gravity across a substep.
    REQUIRE(fixture.TransformOf(entity).Position().y < 4.0f);
    REQUIRE(fixture.Physics().BodyCount() == 1);
}

TEST_CASE("A static RigidBody never moves", "[physics]")
{
    PhysicsFixture fixture;
    const mir::Entity entity = fixture.Add(glm::vec3(0.0f, 0.0f, 0.0f), kStaticBox);

    fixture.Run(60);

    REQUIRE(fixture.TransformOf(entity).Position() == mir::WorldVec3(0.0));
    REQUIRE(fixture.Physics().BodyCount() == 1);
}

TEST_CASE("Edit mode creates no bodies and moves nothing", "[physics]")
{
    PhysicsFixture fixture(mir::EngineMode::Edit);
    const mir::Entity entity = fixture.Add(glm::vec3(0.0f, 5.0f, 0.0f), kDynamicSphere);

    fixture.Run(60);

    REQUIRE(fixture.TransformOf(entity).Position() == mir::WorldVec3(0.0, 5.0, 0.0));
    REQUIRE(fixture.Physics().BodyCount() == 0);
}

TEST_CASE("Pausing Play stops the simulation", "[physics]")
{
    PhysicsFixture fixture;
    const mir::Entity entity = fixture.Add(glm::vec3(0.0f, 5.0f, 0.0f), kDynamicSphere);

    fixture.Run(30);
    const double fell = fixture.TransformOf(entity).Position().y;

    mir::SetPlayState(fixture.world, mir::PlayState::Paused);
    fixture.Run(60);

    REQUIRE(fixture.TransformOf(entity).Position().y == fell);
}

TEST_CASE("A dynamic body lands on a static one", "[physics]")
{
    PhysicsFixture fixture;
    fixture.Add(glm::vec3(0.0f, 0.0f, 0.0f), kStaticBox);
    const mir::Entity sphere = fixture.Add(glm::vec3(0.0f, 3.0f, 0.0f), kDynamicSphere);

    fixture.Run(180);

    // Floor top is y = 0.5, so a resting sphere sits one radius above it.
    const double restY = fixture.TransformOf(sphere).Position().y;
    REQUIRE(restY > 0.5f);
    REQUIRE(restY < 1.25f);
}

TEST_CASE("Destroying the entity frees its body", "[physics]")
{
    PhysicsFixture fixture;
    const mir::Entity entity = fixture.Add(glm::vec3(0.0f, 5.0f, 0.0f), kDynamicSphere);

    fixture.Run(1);
    REQUIRE(fixture.Physics().BodyCount() == 1);

    // Same path UnloadScene and PlaySnapshot::RestoreWorld take.
    fixture.world.DestroyEntity(entity);
    REQUIRE(fixture.Physics().BodyCount() == 0);
}

TEST_CASE("Removing the RigidBody frees its body", "[physics]")
{
    PhysicsFixture fixture;
    const mir::Entity entity = fixture.Add(glm::vec3(0.0f, 5.0f, 0.0f), kDynamicSphere);

    fixture.Run(1);
    REQUIRE(fixture.Physics().BodyCount() == 1);

    fixture.world.RemoveComponent<mir::RigidBody>(entity);
    CHECK(fixture.Physics().BodyCount() == 0);
    CHECK(fixture.world.IsAlive(entity));
}

TEST_CASE("Each created body is tagged with its entity", "[physics]")
{
    PhysicsFixture fixture;
    const mir::Entity floor = fixture.Add(glm::vec3(0.0f, 0.0f, 0.0f), kStaticBox);
    const mir::Entity sphere = fixture.Add(glm::vec3(0.0f, 3.0f, 0.0f), kDynamicSphere);

    fixture.Run(1);

    const mir::BodyHandle floorBody = fixture.world.GetComponent<mir::RigidBody>(floor)->runtime.body;
    const mir::BodyHandle sphereBody = fixture.world.GetComponent<mir::RigidBody>(sphere)->runtime.body;
    CHECK(fixture.Physics().BodyEntity(floorBody) == floor);
    CHECK(fixture.Physics().BodyEntity(sphereBody) == sphere);
}

namespace
{
    constexpr mir::RigidBody kDynamicBox{
        .motion = mir::PhysicsMotion::Dynamic,
        .shape = mir::PhysicsShape::Box,
        .halfExtent = glm::vec3(0.5f),
    };

    // A child of `parent` with its own local Transform, the way the editor builds one.
    mir::Entity AddChild(PhysicsFixture &fixture, mir::Entity parent, const mir::Transform &local,
        const mir::RigidBody &rigidBody)
    {
        const mir::Entity entity = fixture.world.CreateEntity();
        mir::AddTransform(fixture.world, entity, local, parent);
        fixture.world.AddComponent<mir::RigidBody>(entity, rigidBody);
        return entity;
    }

    mir::Entity AddParent(PhysicsFixture &fixture, const mir::Transform &transform, mir::Entity parent = mir::kNullEntity)
    {
        const mir::Entity entity = fixture.world.CreateEntity();
        mir::AddTransform(fixture.world, entity, transform, parent);
        return entity;
    }

    mir::WorldVec3 WorldPositionOf(PhysicsFixture &fixture, mir::Entity entity)
    {
        return mir::ResolveWorld(fixture.world, entity).position.Meters();
    }

    mir::WorldVec3 BodyAt(PhysicsFixture &fixture, mir::Entity entity)
    {
        const mir::BodyHandle body = fixture.world.GetComponent<mir::RigidBody>(entity)->runtime.body;
        return fixture.Physics().BodyPosition(body).value().Meters();
    }

    // The Y quarter turn that sends +X to -Z.
    const glm::quat kQuarterTurnY = glm::angleAxis(glm::half_pi<float>(), glm::vec3(0.0f, 1.0f, 0.0f));
}

TEST_CASE("A parented body spawns where it appears in the world", "[physics][world]")
{
    PhysicsFixture fixture;
    const mir::Entity parent = AddParent(fixture, mir::Transform{ mir::WorldVec3(10.0, 0.0, 0.0) });
    const mir::Entity child = AddChild(fixture, parent, mir::Transform{ mir::WorldVec3(0.0, 5.0, 0.0) }, kDynamicSphere);

    const mir::WorldVec3 at = BodyAt(fixture, child);
    CHECK(at.x == 10.0);
    CHECK(at.y == 5.0);
    CHECK(at.z == 0.0);
}

TEST_CASE("A body under three nested parents spawns at the composed pose", "[physics][world]")
{
    PhysicsFixture fixture;
    const mir::Entity a = AddParent(fixture, mir::Transform{ mir::WorldVec3(1.0, 0.0, 0.0) });
    const mir::Entity b = AddParent(fixture, mir::Transform{ mir::WorldVec3(0.0, 1.0, 0.0) }, a);
    const mir::Entity c = AddParent(fixture, mir::Transform{ mir::WorldVec3(0.0, 0.0, 1.0), kQuarterTurnY }, b);
    const mir::Entity child = AddChild(fixture, c, mir::Transform{ mir::WorldVec3(2.0, 0.0, 0.0) }, kDynamicSphere);

    // c is turned a quarter about Y, so its local +X lands on world -Z
    const mir::WorldVec3 at = BodyAt(fixture, child);
    CHECK(at.x == Catch::Approx(1.0).margin(1e-5));
    CHECK(at.y == Catch::Approx(1.0).margin(1e-5));
    CHECK(at.z == Catch::Approx(-1.0).margin(1e-5));
}

TEST_CASE("A body under a moved, turned and scaled parent behaves like the same body unparented", "[physics][world]")
{
    PhysicsFixture fixture;
    fixture.Add(glm::vec3(0.0f, 0.0f, 0.0f), kStaticBox);

    const mir::Entity parent =
        AddParent(fixture, mir::Transform{ mir::WorldVec3(5.0, 0.0, 0.0), kQuarterTurnY, glm::vec3(2.0f) });
    const mir::Entity child = AddChild(fixture, parent, mir::Transform{ mir::WorldVec3(1.0, 3.0, 0.0) }, kDynamicBox);

    // the same pose and the half extent the world scale of 2 gives, ten meters over so the two do not touch
    mir::RigidBody doubled = kDynamicBox;
    doubled.halfExtent *= 2.0f;
    const mir::Entity plain = fixture.world.CreateEntity();
    mir::AddTransform(fixture.world, plain, mir::Transform{ mir::WorldVec3(-5.0, 6.0, -2.0), kQuarterTurnY });
    fixture.world.AddComponent<mir::RigidBody>(plain, doubled);

    fixture.Run(180);

    const mir::WorldVec3 parented = WorldPositionOf(fixture, child) - mir::WorldVec3(5.0, 6.0, -2.0);
    const mir::WorldVec3 unparented = WorldPositionOf(fixture, plain) - mir::WorldVec3(-5.0, 6.0, -2.0);
    CHECK(parented.x == Catch::Approx(unparented.x).margin(1e-3));
    CHECK(parented.y == Catch::Approx(unparented.y).margin(1e-3));
    CHECK(parented.z == Catch::Approx(unparented.z).margin(1e-3));
    // both fell from y = 6 onto the floor, which is 0.5 up, under a box of half height 1
    CHECK(WorldPositionOf(fixture, child).y == Catch::Approx(1.5).margin(0.1));
}

TEST_CASE("A parented body writes its result back through the parents", "[physics][world]")
{
    PhysicsFixture fixture;
    fixture.Add(glm::vec3(0.0f, 0.0f, 0.0f), kStaticBox);
    const mir::Entity parent =
        AddParent(fixture, mir::Transform{ mir::WorldVec3(4.0, 0.0, 0.0), kQuarterTurnY, glm::vec3(2.0f) });
    const mir::Entity child = AddChild(fixture, parent, mir::Transform{ mir::WorldVec3(0.0, 4.0, 0.0) }, kDynamicSphere);

    fixture.Run(60);

    const mir::WorldVec3 world = WorldPositionOf(fixture, child);
    const mir::WorldVec3 body = BodyAt(fixture, child);
    CHECK(world.x == Catch::Approx(body.x).margin(1e-4));
    CHECK(world.y == Catch::Approx(body.y).margin(1e-4));
    CHECK(world.z == Catch::Approx(body.z).margin(1e-4));
    // it fell, and its local scale is untouched
    CHECK(body.y < 8.0);
    CHECK(fixture.TransformOf(child).Scale() == glm::vec3(1.0f));
}

TEST_CASE("Moving the parent at runtime carries the child's body", "[physics][world]")
{
    PhysicsFixture fixture;
    const mir::Entity parent = AddParent(fixture, mir::Transform{ mir::WorldVec3(0.0, 0.0, 0.0) });
    const mir::Entity child = AddChild(fixture, parent, mir::Transform{ mir::WorldVec3(2.0, 20.0, 0.0) }, kDynamicSphere);
    fixture.Run(5);

    fixture.world.GetComponent<mir::Transform>(parent)->SetPosition(mir::WorldVec3(0.0, 0.0, 7.0));
    fixture.Run(1);
    CHECK(BodyAt(fixture, child).z == Catch::Approx(7.0).margin(1e-3));
    CHECK(BodyAt(fixture, child).x == Catch::Approx(2.0).margin(1e-3));

    // a quarter turn about Y sends the child's local +X to world -Z, around the parent's origin
    fixture.world.GetComponent<mir::Transform>(parent)->SetRotation(kQuarterTurnY);
    fixture.Run(1);
    CHECK(BodyAt(fixture, child).x == Catch::Approx(0.0).margin(1e-3));
    CHECK(BodyAt(fixture, child).z == Catch::Approx(5.0).margin(1e-3));
}

TEST_CASE("A collider follows the world scale", "[physics][world]")
{
    PhysicsFixture fixture;
    const auto castDown = [&]()
    {
        const mir::WorldPosition origin = mir::WorldPosition::FromMeters(mir::WorldVec3(0.0, 10.0, 0.0)).value();
        return fixture.Physics().RayCast(origin, glm::vec3(0.0f, -1.0f, 0.0f), 20.0f);
    };

    SECTION("a sphere takes the largest axis")
    {
        mir::RigidBody sphere = kDynamicSphere;
        sphere.motion = mir::PhysicsMotion::Static;
        const mir::Entity entity = fixture.world.CreateEntity();
        mir::AddTransform(fixture.world, entity, mir::Transform{ mir::WorldVec3(0.0), {}, glm::vec3(2.0f, 1.0f, 3.0f) });
        fixture.world.AddComponent<mir::RigidBody>(entity, sphere);

        // radius 0.5 times 3 reaches 1.5 above the center, so a ray from 10 hits after 8.5
        const std::optional<mir::RayHit> hit = castDown();
        REQUIRE(hit.has_value());
        CHECK(hit->mDistance == Catch::Approx(8.5f).margin(0.01f));
    }

    SECTION("a box scales per axis")
    {
        mir::RigidBody box = kStaticBox;
        box.halfExtent = glm::vec3(1.0f);
        const mir::Entity entity = fixture.world.CreateEntity();
        mir::AddTransform(fixture.world, entity, mir::Transform{ mir::WorldVec3(0.0), {}, glm::vec3(1.0f, 3.0f, 1.0f) });
        fixture.world.AddComponent<mir::RigidBody>(entity, box);

        // half height 1 times 3 puts the top 3 up
        const std::optional<mir::RayHit> hit = castDown();
        REQUIRE(hit.has_value());
        CHECK(hit->mDistance == Catch::Approx(7.0f).margin(0.01f));
    }
}

TEST_CASE("A rotated non-uniform parent stays finite", "[physics][world]")
{
    PhysicsFixture fixture;
    fixture.Add(glm::vec3(0.0f, 0.0f, 0.0f), kStaticBox);
    const mir::Entity parent = AddParent(fixture,
        mir::Transform{ mir::WorldVec3(0.0, 0.0, 0.0), glm::angleAxis(0.7f, glm::vec3(0.0f, 0.0f, 1.0f)), glm::vec3(1.0f, 3.0f, 1.0f) });
    const mir::Entity child = AddChild(fixture, parent, mir::Transform{ mir::WorldVec3(0.0, 2.0, 0.0) }, kDynamicSphere);

    fixture.Run(30);

    const mir::WorldVec3 at = BodyAt(fixture, child);
    CHECK(std::isfinite(at.x));
    CHECK(std::isfinite(at.y));
    CHECK(std::isfinite(at.z));
}

namespace
{
    constexpr mir::RigidBody kKinematicBox{
        .motion = mir::PhysicsMotion::Kinematic,
        .shape = mir::PhysicsShape::Box,
        .halfExtent = glm::vec3(0.5f),
    };

    mir::BodyHandle BodyOfEntity(PhysicsFixture &fixture, mir::Entity entity)
    {
        return fixture.world.GetComponent<mir::RigidBody>(entity)->runtime.body;
    }
}

TEST_CASE("A kinematic body ignores gravity and moves by its velocity", "[physics][kinematic]")
{
    PhysicsFixture fixture;
    const mir::Entity entity = fixture.Add(glm::vec3(0.0f, 5.0f, 0.0f), kKinematicBox);

    fixture.Run(30);
    CHECK(fixture.TransformOf(entity).Position().y == Catch::Approx(5.0).margin(1e-5));

    fixture.Physics().SetBodyLinearVelocity(BodyOfEntity(fixture, entity), glm::vec3(3.0f, 0.0f, 0.0f));
    fixture.Run(60);
    CHECK(fixture.TransformOf(entity).Position().x == Catch::Approx(3.0).margin(0.05));
}

TEST_CASE("A kinematic body pushes a dynamic one and is not pushed back", "[physics][kinematic]")
{
    PhysicsFixture fixture;
    fixture.Add(glm::vec3(0.0f, 0.0f, 0.0f), kStaticBox);
    const mir::Entity sphere = fixture.Add(glm::vec3(0.0f, 1.0f, 0.0f), kDynamicSphere);
    const mir::Entity pusher = fixture.Add(glm::vec3(-3.0f, 1.0f, 0.0f), kKinematicBox);
    fixture.Physics().SetBodyLinearVelocity(BodyOfEntity(fixture, pusher), glm::vec3(2.0f, 0.0f, 0.0f));

    fixture.Run(180);

    // three seconds at 2 m/s carries the box from -3 to 3, whatever it met on the way
    CHECK(fixture.TransformOf(pusher).Position().x == Catch::Approx(3.0).margin(0.05));
    CHECK(fixture.TransformOf(sphere).Position().x > 1.0);
}

TEST_CASE("A Transform write moves a kinematic body to the target and it stays there", "[physics][kinematic]")
{
    PhysicsFixture fixture;
    const mir::Entity entity = fixture.Add(glm::vec3(0.0f, 5.0f, 0.0f), kKinematicBox);
    fixture.Run(2);

    // the first write after the body exists teleports it
    fixture.world.GetComponent<mir::Transform>(entity)->SetPosition(mir::WorldVec3(10.0, 5.0, 0.0));
    fixture.Run(1);
    CHECK(fixture.Physics().BodyPosition(BodyOfEntity(fixture, entity))->Meters().x == Catch::Approx(10.0).margin(1e-4));

    // later writes sweep: the target is reached by the end of the step, with velocity to show for it
    fixture.world.GetComponent<mir::Transform>(entity)->SetPosition(mir::WorldVec3(11.0, 5.0, 0.0));
    fixture.Run(1);
    CHECK(fixture.Physics().BodyPosition(BodyOfEntity(fixture, entity))->Meters().x == Catch::Approx(11.0).margin(1e-3));

    // and it does not carry on past the target
    fixture.Run(10);
    CHECK(fixture.Physics().BodyPosition(BodyOfEntity(fixture, entity))->Meters().x == Catch::Approx(11.0).margin(1e-3));
    CHECK(fixture.Physics().BodyLinearVelocity(BodyOfEntity(fixture, entity)).x == Catch::Approx(0.0).margin(1e-4));
}

TEST_CASE("A kinematic target move pushes a dynamic body in the way", "[physics][kinematic]")
{
    PhysicsFixture fixture;
    fixture.Add(glm::vec3(0.0f, 0.0f, 0.0f), kStaticBox);
    const mir::Entity sphere = fixture.Add(glm::vec3(0.0f, 1.0f, 0.0f), kDynamicSphere);
    const mir::Entity pusher = fixture.Add(glm::vec3(-3.0f, 1.0f, 0.0f), kKinematicBox);
    fixture.Run(2);

    // a platform walked along by Transform writes, one step at a time
    double x = -3.0;
    for (int i = 0; i < 150; ++i)
    {
        x += 0.04;
        fixture.world.GetComponent<mir::Transform>(pusher)->SetPosition(mir::WorldVec3(x, 1.0, 0.0));
        fixture.Run(1);
    }

    CHECK(fixture.TransformOf(sphere).Position().x > 1.0);
    CHECK(fixture.TransformOf(pusher).Position().x == Catch::Approx(x).margin(1e-3));
}

TEST_CASE("A position write on a kinematic body keeps its own rotation", "[physics][kinematic]")
{
    PhysicsFixture fixture;
    const mir::Entity entity = fixture.Add(glm::vec3(0.0f, 5.0f, 0.0f), kKinematicBox);
    fixture.Run(2);
    fixture.world.GetComponent<mir::Transform>(entity)->SetPosition(mir::WorldVec3(1.0, 5.0, 0.0));
    fixture.Run(1);

    const glm::quat turned = glm::angleAxis(0.8f, glm::vec3(0.0f, 1.0f, 0.0f));
    fixture.world.GetComponent<mir::Transform>(entity)->SetRotation(turned);
    fixture.Run(1);
    CHECK(std::abs(glm::dot(fixture.Physics().BodyRotation(BodyOfEntity(fixture, entity)), turned)) > 0.99999f);

    fixture.world.GetComponent<mir::Transform>(entity)->SetPosition(mir::WorldVec3(2.0, 5.0, 0.0));
    fixture.Run(1);
    CHECK(std::abs(glm::dot(fixture.Physics().BodyRotation(BodyOfEntity(fixture, entity)), turned)) > 0.99999f);
    CHECK(fixture.Physics().BodyPosition(BodyOfEntity(fixture, entity))->Meters().x == Catch::Approx(2.0).margin(1e-3));
}

TEST_CASE("A capsule collides at its real size along its axis", "[physics][capsule]")
{
    PhysicsFixture fixture;
    const auto castDown = [&]()
    {
        const mir::WorldPosition origin = mir::WorldPosition::FromMeters(mir::WorldVec3(0.0, 10.0, 0.0)).value();
        return fixture.Physics().RayCast(origin, glm::vec3(0.0f, -1.0f, 0.0f), 20.0f);
    };

    mir::RigidBody capsule{ .motion = mir::PhysicsMotion::Static, .shape = mir::PhysicsShape::Capsule, .radius = 0.5f };
    capsule.halfHeight = 2.0f;

    SECTION("standing along Y, half the total height reaches the top")
    {
        fixture.Add(glm::vec3(0.0f), capsule);
        const std::optional<mir::RayHit> hit = castDown();
        REQUIRE(hit.has_value());
        CHECK(hit->mDistance == Catch::Approx(8.0f).margin(0.01f));
    }

    SECTION("lying along X, only the radius is up")
    {
        capsule.axis = mir::PhysicsAxis::X;
        fixture.Add(glm::vec3(0.0f), capsule);
        const std::optional<mir::RayHit> hit = castDown();
        REQUIRE(hit.has_value());
        CHECK(hit->mDistance == Catch::Approx(9.5f).margin(0.01f));
    }

    SECTION("a half height under the radius is raised to a sphere")
    {
        capsule.halfHeight = 0.1f;
        fixture.Add(glm::vec3(0.0f), capsule);
        const std::optional<mir::RayHit> hit = castDown();
        REQUIRE(hit.has_value());
        CHECK(hit->mDistance == Catch::Approx(9.5f).margin(0.01f));
    }

    SECTION("scaled, the height follows the axis and the radius the other two")
    {
        mir::RigidBody scaledCapsule = capsule;
        const mir::Entity entity = fixture.world.CreateEntity();
        mir::AddTransform(fixture.world, entity, mir::Transform{ mir::WorldVec3(0.0), {}, glm::vec3(3.0f, 2.0f, 1.0f) });
        fixture.world.AddComponent<mir::RigidBody>(entity, scaledCapsule);
        const std::optional<mir::RayHit> hit = castDown();
        REQUIRE(hit.has_value());
        // half height 2 along Y times 2
        CHECK(hit->mDistance == Catch::Approx(6.0f).margin(0.01f));
    }
}

TEST_CASE("The collider offset and its rotation move the shape and not the body", "[physics][offset]")
{
    PhysicsFixture fixture;
    const auto castDown = [&]()
    {
        const mir::WorldPosition origin = mir::WorldPosition::FromMeters(mir::WorldVec3(0.0, 10.0, 0.0)).value();
        return fixture.Physics().RayCast(origin, glm::vec3(0.0f, -1.0f, 0.0f), 20.0f);
    };

    SECTION("an offset lifts the shape")
    {
        mir::RigidBody sphere{ .motion = mir::PhysicsMotion::Static, .shape = mir::PhysicsShape::Sphere, .radius = 0.5f };
        sphere.offset = glm::vec3(0.0f, 3.0f, 0.0f);
        const mir::Entity entity = fixture.Add(glm::vec3(0.0f), sphere);

        const std::optional<mir::RayHit> hit = castDown();
        REQUIRE(hit.has_value());
        CHECK(hit->mDistance == Catch::Approx(6.5f).margin(0.01f));
        // the body itself stays at the entity
        CHECK(fixture.Physics().BodyPosition(BodyOfEntity(fixture, entity))->Meters().y == 0.0);
    }

    SECTION("the offset follows the world scale")
    {
        mir::RigidBody sphere{ .motion = mir::PhysicsMotion::Static, .shape = mir::PhysicsShape::Sphere, .radius = 0.5f };
        sphere.offset = glm::vec3(0.0f, 1.0f, 0.0f);
        const mir::Entity entity = fixture.world.CreateEntity();
        mir::AddTransform(fixture.world, entity, mir::Transform{ mir::WorldVec3(0.0), {}, glm::vec3(2.0f) });
        fixture.world.AddComponent<mir::RigidBody>(entity, sphere);

        // offset 2, radius 1
        const std::optional<mir::RayHit> hit = castDown();
        REQUIRE(hit.has_value());
        CHECK(hit->mDistance == Catch::Approx(7.0f).margin(0.01f));
    }

    SECTION("a rotation stands a long box up")
    {
        mir::RigidBody box{ .motion = mir::PhysicsMotion::Static, .shape = mir::PhysicsShape::Box };
        box.halfExtent = glm::vec3(2.0f, 0.5f, 0.5f);
        box.rotationOffset = glm::angleAxis(glm::half_pi<float>(), glm::vec3(0.0f, 0.0f, 1.0f));
        fixture.Add(glm::vec3(0.0f), box);

        const std::optional<mir::RayHit> hit = castDown();
        REQUIRE(hit.has_value());
        CHECK(hit->mDistance == Catch::Approx(8.0f).margin(0.01f));
    }
}

TEST_CASE("Mass, gravity factor, friction, restitution and sleeping reach the body", "[physics][properties]")
{
    SECTION("mass sets how far an impulse carries a body")
    {
        PhysicsFixture fixture;
        mir::RigidBody heavy = kDynamicSphere;
        heavy.mass = 10.0f;
        heavy.gravityFactor = 0.0f;
        const mir::Entity entity = fixture.Add(glm::vec3(0.0f, 5.0f, 0.0f), heavy);

        fixture.Physics().AddImpulse(BodyOfEntity(fixture, entity), glm::vec3(10.0f, 0.0f, 0.0f));
        fixture.Run(1);

        // 10 kg pushed by 10 N s is 1 m/s, and with no gravity nothing else moves it
        CHECK(fixture.Physics().BodyLinearVelocity(BodyOfEntity(fixture, entity)).x == Catch::Approx(1.0f).margin(0.01f));
        CHECK(fixture.Physics().BodyLinearVelocity(BodyOfEntity(fixture, entity)).y == Catch::Approx(0.0f).margin(1e-4f));
    }

    SECTION("restitution bounces a ball")
    {
        PhysicsFixture fixture;
        fixture.Add(glm::vec3(0.0f), kStaticBox);
        mir::RigidBody bouncy = kDynamicSphere;
        bouncy.restitution = 0.9f;
        mir::RigidBody dead = kDynamicSphere;
        dead.restitution = 0.0f;
        const mir::Entity a = fixture.Add(glm::vec3(-5.0f, 3.0f, 0.0f), bouncy);
        const mir::Entity b = fixture.Add(glm::vec3(5.0f, 3.0f, 0.0f), dead);

        double highestBouncy = 0.0;
        double highestDead = 0.0;
        for (int i = 0; i < 120; ++i)
        {
            fixture.Run(1);
            if (i > 50)
            {
                highestBouncy = std::max(highestBouncy, fixture.TransformOf(a).Position().y);
                highestDead = std::max(highestDead, fixture.TransformOf(b).Position().y);
            }
        }
        CHECK(highestBouncy > 1.8);
        CHECK(highestDead < 1.2);
    }

    SECTION("friction slows a slide")
    {
        PhysicsFixture fixture;
        mir::RigidBody floor = kStaticBox;
        floor.friction = 1.0f;
        fixture.Add(glm::vec3(0.0f), floor);
        mir::RigidBody grippy = kDynamicSphere;
        mir::RigidBody slick = kDynamicSphere;
        grippy.friction = 1.0f;
        slick.friction = 0.0f;
        // boxes slide where spheres roll
        grippy.shape = mir::PhysicsShape::Box;
        slick.shape = mir::PhysicsShape::Box;
        grippy.halfExtent = slick.halfExtent = glm::vec3(0.5f);
        const mir::Entity a = fixture.Add(glm::vec3(-5.0f, 1.01f, 0.0f), grippy);
        const mir::Entity b = fixture.Add(glm::vec3(5.0f, 1.01f, 0.0f), slick);
        fixture.Physics().SetBodyLinearVelocity(BodyOfEntity(fixture, a), glm::vec3(3.0f, 0.0f, 0.0f));
        fixture.Physics().SetBodyLinearVelocity(BodyOfEntity(fixture, b), glm::vec3(3.0f, 0.0f, 0.0f));

        fixture.Run(60);

        CHECK(fixture.TransformOf(a).Position().x + 5.0 < fixture.TransformOf(b).Position().x - 5.0);
    }

    SECTION("a body that cannot sleep stays awake at rest, and Wake wakes one that does")
    {
        PhysicsFixture fixture;
        fixture.Add(glm::vec3(0.0f), kStaticBox);
        mir::RigidBody awake = kDynamicSphere;
        awake.allowSleeping = false;
        const mir::Entity restless = fixture.Add(glm::vec3(-5.0f, 1.0f, 0.0f), awake);
        const mir::Entity sleeper = fixture.Add(glm::vec3(5.0f, 1.0f, 0.0f), kDynamicSphere);

        fixture.Run(300);

        CHECK_FALSE(fixture.Physics().IsBodySleeping(BodyOfEntity(fixture, restless)));
        REQUIRE(fixture.Physics().IsBodySleeping(BodyOfEntity(fixture, sleeper)));

        fixture.Physics().WakeBody(BodyOfEntity(fixture, sleeper));
        CHECK_FALSE(fixture.Physics().IsBodySleeping(BodyOfEntity(fixture, sleeper)));
    }
}

TEST_CASE("A trigger RigidBody blocks nothing and reports with detectStay", "[physics][trigger]")
{
    PhysicsFixture fixture;
    mir::RigidBody zone = kStaticBox;
    zone.isTrigger = true;
    zone.detectStay = true;
    zone.halfExtent = glm::vec3(2.0f, 2.0f, 2.0f);
    fixture.Add(glm::vec3(0.0f, 0.0f, 0.0f), zone);
    const mir::Entity ball = fixture.Add(glm::vec3(0.0f, 3.0f, 0.0f), kDynamicSphere);

    std::size_t stays = 0;
    for (int i = 0; i < 120; ++i)
    {
        fixture.Run(1);
        for (const mir::CollisionEvent &event : fixture.Physics().DrainCollisionEvents())
            stays += event.mPhase == mir::CollisionPhase::Stay ? 1 : 0;
    }

    CHECK(fixture.TransformOf(ball).Position().y < -3.0);
    CHECK(stays > 0);
}

TEST_CASE("A RigidBody gets its body when it is added, before any step", "[physics]")
{
    PhysicsFixture fixture;
    const mir::Entity entity = fixture.Add(glm::vec3(0.0f, 5.0f, 0.0f), kDynamicSphere);

    const mir::BodyHandle body = fixture.world.GetComponent<mir::RigidBody>(entity)->runtime.body;
    REQUIRE(body.IsValid());
    CHECK(fixture.Physics().BodyCount() == 1);
    CHECK(fixture.Physics().BodyEntity(body) == entity);
}

TEST_CASE("A RigidBody can be added, removed and added again", "[physics]")
{
    PhysicsFixture fixture;
    const mir::Entity entity = fixture.Add(glm::vec3(0.0f, 5.0f, 0.0f), kDynamicSphere);
    REQUIRE(fixture.Physics().BodyCount() == 1);

    fixture.world.RemoveComponent<mir::RigidBody>(entity);
    REQUIRE(fixture.Physics().BodyCount() == 0);

    fixture.world.AddComponent<mir::RigidBody>(entity, kDynamicSphere);
    CHECK(fixture.Physics().BodyCount() == 1);
    CHECK(fixture.world.GetComponent<mir::RigidBody>(entity)->runtime.body.IsValid());
}

TEST_CASE("A velocity set right after the add moves the body on the first step", "[physics]")
{
    PhysicsFixture fixture;
    const mir::Entity entity = fixture.Add(glm::vec3(0.0f, 5.0f, 0.0f), kDynamicSphere);

    // what a script's OnStart does
    fixture.Physics().SetBodyLinearVelocity(
        fixture.world.GetComponent<mir::RigidBody>(entity)->runtime.body, glm::vec3(6.0f, 0.0f, 0.0f));
    fixture.Run(6);

    CHECK(fixture.TransformOf(entity).Position().x > 0.4);
}

TEST_CASE("Edit mode builds no bodies until PlayBegin, which builds each once", "[physics]")
{
    PhysicsFixture fixture(mir::EngineMode::Edit);
    fixture.Add(glm::vec3(0.0f, 5.0f, 0.0f), kDynamicSphere);
    fixture.Add(glm::vec3(0.0f, 0.0f, 0.0f), kStaticBox);
    REQUIRE(fixture.Physics().BodyCount() == 0);
    REQUIRE_FALSE(fixture.Physics().IsSimulationLive());

    mir::PhysicsSystem::OnPlayBegin(fixture.world);
    CHECK(fixture.Physics().BodyCount() == 2);
    CHECK(fixture.Physics().IsSimulationLive());

    // a body added while live is built by the hook, not by a second pass
    fixture.Add(glm::vec3(3.0f, 5.0f, 0.0f), kDynamicSphere);
    CHECK(fixture.Physics().BodyCount() == 3);

    // a pass over bodies that already exist skips them
    mir::PhysicsSystem::OnPlayEnd(fixture.world);
    mir::PhysicsSystem::OnPlayBegin(fixture.world);
    CHECK(fixture.Physics().BodyCount() == 3);
}

TEST_CASE("After PlayEnd a restore adds RigidBodies without bodies and the next Play builds one each", "[physics]")
{
    PhysicsFixture fixture(mir::EngineMode::Edit);
    fixture.Add(glm::vec3(0.0f, 5.0f, 0.0f), kDynamicSphere);
    mir::PhysicsSystem::OnPlayBegin(fixture.world);
    REQUIRE(fixture.Physics().BodyCount() == 1);

    // Stop: PlayEnd, then the restore destroys the Play entities and adds the saved ones
    mir::PhysicsSystem::OnPlayEnd(fixture.world);
    REQUIRE_FALSE(fixture.Physics().IsSimulationLive());
    std::vector<mir::Entity> playEntities;
    fixture.world.ForEach<mir::RigidBody>([&](mir::Entity entity, mir::RigidBody &) { playEntities.push_back(entity); });
    for (const mir::Entity entity : playEntities)
        fixture.world.DestroyEntity(entity);
    REQUIRE(fixture.Physics().BodyCount() == 0);

    const mir::Entity restored = fixture.Add(glm::vec3(0.0f, 5.0f, 0.0f), kDynamicSphere);
    CHECK(fixture.Physics().BodyCount() == 0);
    CHECK_FALSE(fixture.world.GetComponent<mir::RigidBody>(restored)->runtime.body.IsValid());

    mir::PhysicsSystem::OnPlayBegin(fixture.world);
    CHECK(fixture.Physics().BodyCount() == 1);
}

TEST_CASE("A copied RigidBody gets its own body and not the original's", "[physics]")
{
    PhysicsFixture fixture;
    const mir::Entity original = fixture.Add(glm::vec3(0.0f, 5.0f, 0.0f), kDynamicSphere);
    // by value: adding the copy can move the original's storage
    const mir::RigidBody source = *fixture.world.GetComponent<mir::RigidBody>(original);
    REQUIRE(source.runtime.body.IsValid());

    // the bytes still carry the original's live handle, like Duplicate or a prefab instance
    const mir::Entity copy = fixture.Add(glm::vec3(4.0f, 5.0f, 0.0f), source);
    const mir::BodyHandle copyBody = fixture.world.GetComponent<mir::RigidBody>(copy)->runtime.body;

    REQUIRE(copyBody.IsValid());
    CHECK(copyBody != source.runtime.body);
    CHECK(fixture.Physics().BodyEntity(copyBody) == copy);
    CHECK(fixture.Physics().BodyCount() == 2);

    fixture.world.DestroyEntity(original);
    CHECK(fixture.Physics().BodyCount() == 1);
    CHECK(fixture.Physics().BodyEntity(copyBody) == copy);
}

TEST_CASE("A Transform added after the RigidBody gets its body at the first step", "[physics]")
{
    PhysicsFixture fixture;
    const mir::Entity entity = fixture.world.CreateEntity();
    fixture.world.AddComponent<mir::RigidBody>(entity, kDynamicSphere);
    CHECK(fixture.Physics().BodyCount() == 0);

    mir::AddTransform(fixture.world, entity, mir::Transform{ glm::vec3(0.0f, 5.0f, 0.0f) });
    fixture.Run(1);

    CHECK(fixture.Physics().BodyCount() == 1);
    CHECK(fixture.world.GetComponent<mir::RigidBody>(entity)->runtime.body.IsValid());
}

TEST_CASE("Writing the Transform teleports the body", "[physics]")
{
    PhysicsFixture fixture;
    const mir::Entity entity = fixture.Add(glm::vec3(0.0f, 5.0f, 0.0f), kDynamicSphere);

    fixture.Run(10);
    REQUIRE(fixture.TransformOf(entity).Position().y < 5.0f);

    // What a script that assigns Transform.position looks like from here.
    fixture.world.GetComponent<mir::Transform>(entity)->SetPosition(glm::vec3(3.0f, 10.0f, 0.0f));
    fixture.Run(1);

    const mir::WorldVec3 position = fixture.TransformOf(entity).Position();
    REQUIRE(std::abs(position.x - 3.0) < 0.01);
    REQUIRE(position.y > 9.9f);
}

namespace
{
    // Holds every dynamic body while closed and records where the bodies were when it ran.
    class RecordingGate final : public mir::PhysicsStepGate
    {
      public:
        bool open = false;
        int calls = 0;
        std::vector<mir::WorldVec3> seen;

        void BeforeStep(mir::World &world, mir::PhysicsWorld &physics, float) override
        {
            ++calls;
            seen.clear();
            world.GetOrCreateQuery<mir::RigidBody>().ForEach([&](mir::Entity, mir::RigidBody &rigidBody) {
                const std::optional<mir::WorldPosition> at = physics.BodyPosition(rigidBody.runtime.body);
                REQUIRE(at);
                seen.push_back(at->Meters());
                if (open)
                    physics.ResumeBody(rigidBody.runtime.body);
                else
                    physics.HoldBody(rigidBody.runtime.body);
            });
        }
    };
} // namespace

TEST_CASE("Step gates see new bodies and teleports before the step and can hold a body", "[physics]")
{
    PhysicsFixture fixture;
    RecordingGate gate;
    fixture.world.EmplaceResource<mir::PhysicsStepGates>().mGates.push_back(&gate);
    const mir::Entity entity = fixture.Add(glm::vec3(0.0f, 5.0f, 0.0f), kDynamicSphere);

    fixture.Run(30);
    CHECK(gate.calls == 30);
    REQUIRE(gate.seen.size() == 1);
    CHECK(gate.seen[0] == mir::WorldVec3(0.0, 5.0, 0.0));
    CHECK(fixture.TransformOf(entity).Position() == mir::WorldVec3(0.0, 5.0, 0.0));

    fixture.world.GetComponent<mir::Transform>(entity)->SetPosition(glm::vec3(3.0f, 10.0f, 0.0f));
    fixture.Run(1);
    REQUIRE(gate.seen.size() == 1);
    CHECK(gate.seen[0] == mir::WorldVec3(3.0, 10.0, 0.0));
    CHECK(fixture.TransformOf(entity).Position() == mir::WorldVec3(3.0, 10.0, 0.0));

    gate.open = true;
    fixture.Run(30);
    CHECK(fixture.TransformOf(entity).Position().y < 9.0);
}

namespace
{
    // Map-edge coordinates shared with the terrain ADRs.
    const mir::WorldVec3 kEdgeA{-150000.125, 350.5, 75000.5};
    const mir::WorldVec3 kEdgeB{150000.625, -20.0, -75000.25};
    const mir::WorldVec3 kNudge{0.001, -0.002, 0.003};

    mir::WorldVec3 BodyMeters(PhysicsFixture &fixture, mir::Entity entity)
    {
        const mir::RigidBody *rigidBody = fixture.world.GetComponent<mir::RigidBody>(entity);
        REQUIRE(rigidBody != nullptr);
        const std::optional<mir::WorldPosition> position = fixture.Physics().BodyPosition(rigidBody->runtime.body);
        REQUIRE(position);
        return position->Meters();
    }
}

TEST_CASE("A far Transform reaches its body without losing a millimeter", "[physics][lwc]")
{
    if (!mir::kLargeWorld)
        SKIP("float world positions (MIR_LARGE_WORLD=OFF) cannot hold this");

    PhysicsFixture fixture;
    const mir::Entity box = fixture.Add(kEdgeB + kNudge, kStaticBox);

    fixture.Run(1);
    CHECK(BodyMeters(fixture, box) == kEdgeB + kNudge);
    CHECK(fixture.TransformOf(box).Position() == kEdgeB + kNudge);

    // an outside edit of another millimeter is pushed to the body as is
    REQUIRE(fixture.world.GetComponent<mir::Transform>(box)->Translate(kNudge));
    fixture.Run(1);
    const mir::WorldVec3 moved = (kEdgeB + kNudge) + kNudge;
    CHECK(BodyMeters(fixture, box) == moved);
    CHECK(fixture.TransformOf(box).Position() == moved);
}

TEST_CASE("A far dynamic body writes back what Jolt simulated", "[physics][lwc]")
{
    if (!mir::kLargeWorld)
        SKIP("float world positions (MIR_LARGE_WORLD=OFF) cannot hold this");

    double fell[3];
    const mir::WorldVec3 starts[3] = {mir::WorldVec3(0.0, 5.0, 0.0), kEdgeA, kEdgeB};
    for (int i = 0; i < 3; ++i)
    {
        PhysicsFixture fixture;
        const mir::Entity sphere = fixture.Add(starts[i] + kNudge, kDynamicSphere);

        fixture.Run(30);

        const mir::WorldVec3 position = fixture.TransformOf(sphere).Position();
        CHECK(position == BodyMeters(fixture, sphere));
        // free fall is straight down: the sideways millimeters are untouched
        CHECK(position.x == starts[i].x + kNudge.x);
        CHECK(position.z == starts[i].z + kNudge.z);
        fell[i] = position.y - starts[i].y;
    }

    CHECK(fell[0] < -1.0);
    CHECK(std::abs(fell[1] - fell[0]) <= 1e-9);
    CHECK(std::abs(fell[2] - fell[0]) <= 1e-9);
}

TEST_CASE("A far dynamic body lands on a far static one", "[physics][lwc]")
{
    if (!mir::kLargeWorld)
        SKIP("float world positions (MIR_LARGE_WORLD=OFF) cannot hold this");

    double restHeight[2];
    const mir::WorldVec3 floors[2] = {mir::WorldVec3(0.0), kEdgeA};
    for (int i = 0; i < 2; ++i)
    {
        PhysicsFixture fixture;
        fixture.Add(floors[i], kStaticBox);
        const mir::Entity sphere = fixture.Add(floors[i] + mir::WorldVec3(0.0, 3.0, 0.0), kDynamicSphere);

        fixture.Run(180);

        restHeight[i] = fixture.TransformOf(sphere).Position().y - floors[i].y;
        CHECK(restHeight[i] > 0.5);
        CHECK(restHeight[i] < 1.25);
    }

    CHECK(std::abs(restHeight[1] - restHeight[0]) <= 1e-4);
}
