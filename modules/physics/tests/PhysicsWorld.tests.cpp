/**
 * @file PhysicsWorld.tests.cpp
 * @author Jackey Cen
 * @brief Tests for physics/PhysicsWorld.
 *
 * These also stand in for a Jolt integration test: they are the only place that
 * proves the globals in JoltRuntime.cpp are installed and torn down correctly.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <core/log/LogHistory.h>
#include <physics/PhysicsWorld.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <glm/geometric.hpp>
#include <glm/trigonometric.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <optional>
#include <span>
#include <string>

namespace
{
    constexpr float kStep = 1.0f / 60.0f;

    // Map-edge coordinates shared with the terrain ADRs.
    const mir::WorldVec3 kEdgeA{-150000.125, 350.5, 75000.5};
    const mir::WorldVec3 kEdgeB{150000.625, -20.0, -75000.25};
    const mir::WorldVec3 kNudge{0.001, -0.002, 0.003};

    mir::WorldPosition At(const mir::WorldVec3 &meters)
    {
        const std::optional<mir::WorldPosition> position = mir::WorldPosition::FromMeters(meters);
        REQUIRE(position);
        return *position;
    }

    mir::WorldPosition At(mir::WorldReal x, mir::WorldReal y, mir::WorldReal z) { return At(mir::WorldVec3(x, y, z)); }

    mir::WorldVec3 MetersOf(const mir::PhysicsWorld &world, mir::BodyHandle body)
    {
        const std::optional<mir::WorldPosition> position = world.BodyPosition(body);
        REQUIRE(position);
        return position->Meters();
    }

    double MaxError(const mir::WorldVec3 &actual, const mir::WorldVec3 &expected)
    {
        const mir::WorldVec3 error = glm::abs(actual - expected);
        return std::max({error.x, error.y, error.z});
    }

    void StepFor(mir::PhysicsWorld &world, int steps)
    {
        for (int i = 0; i < steps; ++i)
            world.Step(kStep);
    }

    // A wide, thin slab centred on `center`; its top surface is 0.5 above it.
    mir::BodyHandle AddFloor(mir::PhysicsWorld &world, const mir::WorldVec3 &center = mir::WorldVec3(0.0))
    {
        return world.CreateBody({.mMotion = mir::PhysicsMotion::Static,
                                 .mShape = mir::PhysicsShape::Box,
                                 .mPosition = At(center),
                                 .mHalfExtent = glm::vec3(10.0f, 0.5f, 10.0f)});
    }
}

TEST_CASE("A world with a static body takes a step", "[physics]")
{
    mir::PhysicsWorld world;

    const mir::BodyHandle floor = AddFloor(world);
    REQUIRE(floor.IsValid());
    REQUIRE(world.BodyCount() == 1);

    world.Step(kStep);

    REQUIRE(world.BodyCount() == 1);
    REQUIRE(MetersOf(world, floor).y == 0.0);
}

TEST_CASE("A dynamic sphere falls under gravity", "[physics]")
{
    mir::PhysicsWorld world;

    const mir::BodyHandle sphere = world.CreateBody({.mMotion = mir::PhysicsMotion::Dynamic,
                                                     .mShape = mir::PhysicsShape::Sphere,
                                                     .mPosition = At(0.0, 5.0, 0.0),
                                                     .mRadius = 0.5f});
    REQUIRE(sphere.IsValid());

    StepFor(world, 30);

    // Half a second of 9.81 m/s^2 is ~1.2m; a loose bound keeps this from
    // depending on how Jolt distributes gravity across substeps.
    REQUIRE(MetersOf(world, sphere).y < 4.0);
    REQUIRE(world.BodyLinearVelocity(sphere).y < 0.0f);
}

TEST_CASE("A dynamic sphere comes to rest on a static floor", "[physics]")
{
    mir::PhysicsWorld world;

    AddFloor(world);
    const mir::BodyHandle sphere = world.CreateBody({.mMotion = mir::PhysicsMotion::Dynamic,
                                                     .mShape = mir::PhysicsShape::Sphere,
                                                     .mPosition = At(0.0, 3.0, 0.0),
                                                     .mRadius = 0.5f});
    REQUIRE(world.BodyCount() == 2);

    StepFor(world, 180);

    // Resting means the centre sits one radius above the floor top (y = 1.0).
    const double restY = MetersOf(world, sphere).y;
    REQUIRE(restY > 0.5);  // did not tunnel through
    REQUIRE(restY < 1.25); // actually landed instead of hanging in the air
}

TEST_CASE("Gravity is taken from the description", "[physics]")
{
    mir::PhysicsWorldDesc desc;
    desc.mGravity = glm::vec3(0.0f, 0.0f, 0.0f);

    mir::PhysicsWorld world(desc);

    const mir::BodyHandle sphere = world.CreateBody({.mMotion = mir::PhysicsMotion::Dynamic,
                                                     .mShape = mir::PhysicsShape::Sphere,
                                                     .mPosition = At(0.0, 5.0, 0.0),
                                                     .mRadius = 0.5f});
    StepFor(world, 30);

    REQUIRE(std::abs(MetersOf(world, sphere).y - 5.0) < 0.01);
}

TEST_CASE("DestroyBody drops the body", "[physics]")
{
    mir::PhysicsWorld world;

    const mir::BodyHandle floor = AddFloor(world);
    const mir::BodyHandle sphere = world.CreateBody({.mMotion = mir::PhysicsMotion::Dynamic,
                                                     .mShape = mir::PhysicsShape::Sphere,
                                                     .mPosition = At(0.0, 3.0, 0.0),
                                                     .mRadius = 0.5f});
    REQUIRE(world.BodyCount() == 2);

    world.DestroyBody(sphere);
    REQUIRE(world.BodyCount() == 1);

    // Destroying twice, and destroying an invalid handle, are both no-ops.
    world.DestroyBody(sphere);
    world.DestroyBody(mir::BodyHandle{});
    REQUIRE(world.BodyCount() == 1);

    world.Step(kStep);
    REQUIRE(MetersOf(world, floor).y == 0.0);
}

TEST_CASE("Destroying a body wakes what slept on it", "[physics]")
{
    mir::PhysicsWorld world;

    const mir::BodyHandle floor = AddFloor(world);
    const mir::BodyHandle box = world.CreateBody({.mMotion = mir::PhysicsMotion::Dynamic,
                                                  .mShape = mir::PhysicsShape::Box,
                                                  .mPosition = At(0.0, 1.0, 0.0),
                                                  .mHalfExtent = glm::vec3(0.5f, 0.25f, 0.5f)});

    // Jolt puts a body to sleep after half a second at rest.
    StepFor(world, 120);
    const double restY = MetersOf(world, box).y;
    REQUIRE(world.BodyLinearVelocity(box) == glm::vec3(0.0f));

    // Jolt's own removal leaves a sleeping body where it was, in the air.
    world.DestroyBody(floor);
    StepFor(world, 30);
    CHECK(MetersOf(world, box).y < restY - 0.5);
}

TEST_CASE("RayCast reports the closest hit", "[physics]")
{
    mir::PhysicsWorld world;

    const mir::BodyHandle floor = AddFloor(world);
    const glm::vec3 down(0.0f, -2.0f, 0.0f);
    const std::optional<mir::RayHit> hit = world.RayCast(At(1.0, 5.0, 2.0), down, 10.0f);
    REQUIRE(hit);
    CHECK(hit->mBody == floor);
    CHECK(MaxError(hit->mPoint.Meters(), mir::WorldVec3(1.0, 0.5, 2.0)) <= 1e-5);
    CHECK(glm::length(hit->mNormal - glm::vec3(0.0f, 1.0f, 0.0f)) <= 1e-5f);
    CHECK(std::abs(hit->mDistance - 4.5f) <= 1e-5f);

    // A body in the way is hit first.
    const mir::BodyHandle sphere = world.CreateBody({.mMotion = mir::PhysicsMotion::Dynamic,
                                                     .mShape = mir::PhysicsShape::Sphere,
                                                     .mPosition = At(1.0, 2.0, 2.0),
                                                     .mRadius = 0.5f});
    const std::optional<mir::RayHit> first = world.RayCast(At(1.0, 5.0, 2.0), down, 10.0f);
    REQUIRE(first);
    CHECK(first->mBody == sphere);
    CHECK(std::abs(first->mPoint.Meters().y - 2.5) <= 1e-5);

    // Short of everything, pointing away, or malformed: no hit.
    CHECK_FALSE(world.RayCast(At(1.0, 5.0, 2.0), down, 2.0f));
    CHECK_FALSE(world.RayCast(At(1.0, 5.0, 2.0), -down, 10.0f));
    CHECK_FALSE(world.RayCast(At(1.0, 5.0, 2.0), glm::vec3(0.0f), 10.0f));
    CHECK_FALSE(world.RayCast(At(1.0, 5.0, 2.0), glm::vec3(std::nanf(""), -1.0f, 0.0f), 10.0f));
    CHECK_FALSE(world.RayCast(At(1.0, 5.0, 2.0), down, 0.0f));
    CHECK_FALSE(world.RayCast(At(1.0, 5.0, 2.0), down, -1.0f));
}

TEST_CASE("RayCast keeps a millimeter at the map edge", "[physics][lwc]")
{
    if (!mir::kLargeWorld)
        SKIP("float world positions (MIR_LARGE_WORLD=OFF) cannot hold this");

    for (const mir::WorldVec3 &edge : {kEdgeA, kEdgeB})
    {
        mir::PhysicsWorld world;
        AddFloor(world, edge);

        const mir::WorldVec3 from = edge + kNudge + mir::WorldVec3(0.0, 5.0, 0.0);
        const std::optional<mir::RayHit> hit = world.RayCast(At(from), glm::vec3(0.0f, -1.0f, 0.0f), 10.0f);
        REQUIRE(hit);
        // straight down, so x and z come back bit for bit
        CHECK(hit->mPoint.Meters().x == from.x);
        CHECK(hit->mPoint.Meters().z == from.z);
        CHECK(std::abs(hit->mPoint.Meters().y - (edge.y + 0.5)) <= 1e-5);
    }
}

TEST_CASE("A bad handle has no position instead of crashing", "[physics]")
{
    mir::PhysicsWorld world;

    const mir::BodyHandle invalid;
    REQUIRE_FALSE(invalid.IsValid());
    REQUIRE_FALSE(world.BodyPosition(invalid));
    REQUIRE(world.BodyLinearVelocity(invalid) == glm::vec3(0.0f));

    // the Scene origin is a real place, so a body that is gone must not report it
    const mir::BodyHandle destroyed = world.CreateBody({.mMotion = mir::PhysicsMotion::Dynamic,
                                                        .mShape = mir::PhysicsShape::Sphere,
                                                        .mPosition = At(0.0, 3.0, 0.0),
                                                        .mRadius = 0.5f});
    world.DestroyBody(destroyed);
    REQUIRE_FALSE(world.BodyPosition(destroyed));
}

TEST_CASE("A body carries the entity it was tagged with", "[physics]")
{
    mir::PhysicsWorld world;

    const mir::BodyHandle floor = AddFloor(world);
    const mir::BodyHandle sphere = world.CreateBody({.mMotion = mir::PhysicsMotion::Dynamic,
                                                     .mShape = mir::PhysicsShape::Sphere,
                                                     .mPosition = At(0.0, 3.0, 0.0),
                                                     .mRadius = 0.5f});
    CHECK(world.BodyEntity(floor) == mir::kNullEntity);

    // Entity{0, 0} packs to Jolt's default user data.
    world.SetBodyEntity(floor, mir::Entity{ 0, 0 });
    CHECK(world.BodyEntity(floor) == mir::Entity{ 0, 0 });

    const mir::Entity high{ 0xFFFFFFFEu, 0xFFFFFFFFu };
    world.SetBodyEntity(sphere, high);
    CHECK(world.BodyEntity(sphere) == high);
    CHECK(world.BodyEntity(floor) == mir::Entity{ 0, 0 });

    const std::optional<mir::RayHit> hit = world.RayCast(At(0.0, 5.0, 0.0), glm::vec3(0.0f, -1.0f, 0.0f), 10.0f);
    REQUIRE(hit);
    CHECK(world.BodyEntity(hit->mBody) == high);

    world.SetBodyEntity(floor, mir::kNullEntity);
    CHECK(world.BodyEntity(floor) == mir::kNullEntity);

    world.SetBodyEntity(mir::BodyHandle{}, high);
    CHECK(world.BodyEntity(mir::BodyHandle{}) == mir::kNullEntity);

    world.DestroyBody(sphere);
    CHECK(world.BodyEntity(sphere) == mir::kNullEntity);
    world.SetBodyEntity(sphere, high);
    CHECK(world.BodyEntity(sphere) == mir::kNullEntity);

    // A new body in the destroyed one's slot starts untagged.
    const mir::BodyHandle reused = world.CreateBody({.mMotion = mir::PhysicsMotion::Dynamic,
                                                     .mShape = mir::PhysicsShape::Sphere,
                                                     .mPosition = At(0.0, 3.0, 0.0),
                                                     .mRadius = 0.5f});
    CHECK(world.BodyEntity(reused) == mir::kNullEntity);
}

TEST_CASE("Degenerate shapes become empty colliders, not asserted on", "[physics]")
{
    mir::PhysicsWorld world;

    const mir::BodyHandle sphere = world.CreateBody({.mMotion = mir::PhysicsMotion::Static,
                                                     .mShape = mir::PhysicsShape::Sphere,
                                                     .mPosition = mir::WorldPosition{},
                                                     .mRadius = 0.0f});
    const mir::BodyHandle negative = world.CreateBody({.mMotion = mir::PhysicsMotion::Dynamic,
                                                       .mShape = mir::PhysicsShape::Sphere,
                                                       .mPosition = mir::WorldPosition{},
                                                       .mRadius = -1.0f});
    const mir::BodyHandle flat = world.CreateBody({.mMotion = mir::PhysicsMotion::Static,
                                                   .mShape = mir::PhysicsShape::Box,
                                                   .mPosition = mir::WorldPosition{},
                                                   .mHalfExtent = glm::vec3(1.0f, 0.0f, 1.0f)});

    // each is a body with a handle that nothing can hit
    CHECK(sphere.IsValid());
    CHECK(negative.IsValid());
    CHECK(flat.IsValid());
    CHECK(world.BodyCount() == 3);
    CHECK_FALSE(world.RayCast(mir::WorldPosition::FromMeters(mir::WorldVec3(0.0, 5.0, 0.0)).value(),
        glm::vec3(0.0f, -1.0f, 0.0f), 10.0f).has_value());
}

TEST_CASE("A non-positive step does nothing", "[physics]")
{
    mir::PhysicsWorld world;

    const mir::BodyHandle sphere = world.CreateBody({.mMotion = mir::PhysicsMotion::Dynamic,
                                                     .mShape = mir::PhysicsShape::Sphere,
                                                     .mPosition = At(0.0, 5.0, 0.0),
                                                     .mRadius = 0.5f});

    world.Step(0.0f);
    world.Step(-1.0f);

    REQUIRE(MetersOf(world, sphere).y == 5.0);
    REQUIRE(world.BodyLinearVelocity(sphere).y == 0.0f);
}

TEST_CASE("Several worlds can exist at once", "[physics]")
{
    {
        mir::PhysicsWorld first;
        mir::PhysicsWorld second;

        const mir::BodyHandle a = first.CreateBody({.mMotion = mir::PhysicsMotion::Dynamic,
                                                    .mShape = mir::PhysicsShape::Sphere,
                                                    .mPosition = At(0.0, 5.0, 0.0),
                                                    .mRadius = 0.5f});
        const mir::BodyHandle b = second.CreateBody({.mMotion = mir::PhysicsMotion::Static,
                                                     .mShape = mir::PhysicsShape::Sphere,
                                                     .mPosition = At(0.0, 5.0, 0.0),
                                                     .mRadius = 0.5f});

        StepFor(first, 30);
        StepFor(second, 30);

        REQUIRE(first.BodyCount() == 1);
        REQUIRE(second.BodyCount() == 1);
        REQUIRE(MetersOf(first, a).y < 5.0);
        REQUIRE(MetersOf(second, b).y == 5.0);

        // A handle only means something to the world that issued it.
        REQUIRE(second.BodyCount() == 1);
    }

    // Both refs are gone, so the Jolt globals were torn down; building another
    // world has to bring them back up.
    mir::PhysicsWorld third;
    REQUIRE(third.CreateBody({.mMotion = mir::PhysicsMotion::Dynamic,
                              .mShape = mir::PhysicsShape::Sphere,
                              .mPosition = At(0.0, 5.0, 0.0),
                              .mRadius = 0.5f}).IsValid());
    third.Step(kStep);
}

TEST_CASE("A body keeps a millimeter at the map edge", "[physics][lwc]")
{
    if (!mir::kLargeWorld)
        SKIP("float world positions (MIR_LARGE_WORLD=OFF) cannot hold this");

    for (const mir::WorldVec3 &edge : {kEdgeA, kEdgeB})
    {
        mir::PhysicsWorld world;

        // fails with a float Jolt, or with a float cast anywhere on the way in or out
        const mir::BodyHandle body = world.CreateBody({.mMotion = mir::PhysicsMotion::Static,
                                                       .mShape = mir::PhysicsShape::Sphere,
                                                       .mPosition = At(edge + kNudge),
                                                       .mRadius = 0.5f});
        CHECK(MetersOf(world, body) == edge + kNudge);

        world.SetBodyTransform(body, At(edge + kNudge + kNudge), mir::kIdentityRotation);
        CHECK(MetersOf(world, body) == edge + kNudge + kNudge);

        world.Step(kStep);
        CHECK(MetersOf(world, body) == edge + kNudge + kNudge);

        // as a float world position the nudge is gone
        CHECK(static_cast<float>(edge.x + kNudge.x) == static_cast<float>(edge.x));
    }
}

TEST_CASE("A slow body at the map edge moves as it does at the origin", "[physics][lwc]")
{
    if (!mir::kLargeWorld)
        SKIP("float world positions (MIR_LARGE_WORLD=OFF) cannot hold this");

    // 0.05 m/s is 0.83 mm a step, far below the 1/64 m spacing of a float out here
    mir::WorldVec3 travelled[3];
    const mir::WorldVec3 starts[3] = {mir::WorldVec3(0.0), kEdgeA, kEdgeB};
    for (int i = 0; i < 3; ++i)
    {
        mir::PhysicsWorldDesc desc;
        desc.mGravity = glm::vec3(0.0f);
        mir::PhysicsWorld world(desc);

        const mir::BodyHandle sphere = world.CreateBody({.mMotion = mir::PhysicsMotion::Dynamic,
                                                         .mShape = mir::PhysicsShape::Sphere,
                                                         .mPosition = At(starts[i]),
                                                         .mRadius = 0.5f});
        world.SetBodyLinearVelocity(sphere, glm::vec3(0.05f, 0.0f, -0.025f));
        StepFor(world, 20);

        travelled[i] = MetersOf(world, sphere) - starts[i];
    }

    CHECK(travelled[0].x > 0.015);
    CHECK(travelled[0].z < -0.0075);
    CHECK(MaxError(travelled[1], travelled[0]) <= 1e-9);
    CHECK(MaxError(travelled[2], travelled[0]) <= 1e-9);
}

TEST_CASE("Bodies collide at the map edge as they do at the origin", "[physics][lwc]")
{
    if (!mir::kLargeWorld)
        SKIP("float world positions (MIR_LARGE_WORLD=OFF) cannot hold this");

    double restHeight[3];
    const mir::WorldVec3 floors[3] = {mir::WorldVec3(0.0), kEdgeA, kEdgeB};
    for (int i = 0; i < 3; ++i)
    {
        mir::PhysicsWorld world;

        AddFloor(world, floors[i]);
        const mir::BodyHandle sphere = world.CreateBody({.mMotion = mir::PhysicsMotion::Dynamic,
                                                         .mShape = mir::PhysicsShape::Sphere,
                                                         .mPosition = At(floors[i] + mir::WorldVec3(0.0, 3.0, 0.0)),
                                                         .mRadius = 0.5f});
        StepFor(world, 180);

        const mir::WorldVec3 rest = MetersOf(world, sphere) - floors[i];
        restHeight[i] = rest.y;

        // landed on the floor, and did not drift sideways
        CHECK(rest.y > 0.5);
        CHECK(rest.y < 1.25);
        CHECK(std::abs(rest.x) <= 1e-3);
        CHECK(std::abs(rest.z) <= 1e-3);
    }

    CHECK(std::abs(restHeight[1] - restHeight[0]) <= 1e-4);
    CHECK(std::abs(restHeight[2] - restHeight[0]) <= 1e-4);
}

TEST_CASE("A held body keeps its pose and its velocities until it resumes", "[physics][hold]")
{
    mir::PhysicsWorld world;
    AddFloor(world);
    const mir::BodyHandle sphere = world.CreateBody({.mMotion = mir::PhysicsMotion::Dynamic,
                                                     .mShape = mir::PhysicsShape::Sphere,
                                                     .mPosition = At(0.0, 6.0, 0.0),
                                                     .mRadius = 0.5f});
    world.SetBodyLinearVelocity(sphere, glm::vec3(1.0f, -2.0f, 0.5f));
    world.SetBodyAngularVelocity(sphere, glm::vec3(0.0f, 3.0f, 0.0f));
    const mir::WorldVec3 before = MetersOf(world, sphere);

    world.HoldBody(sphere);
    CHECK(world.IsBodyHeld(sphere));
    StepFor(world, 60);
    CHECK(MetersOf(world, sphere) == before);
    CHECK(world.BodyLinearVelocity(sphere) == glm::vec3(1.0f, -2.0f, 0.5f));
    CHECK(world.BodyAngularVelocity(sphere) == glm::vec3(0.0f, 3.0f, 0.0f));

    world.ResumeBody(sphere);
    CHECK_FALSE(world.IsBodyHeld(sphere));
    CHECK(world.BodyLinearVelocity(sphere) == glm::vec3(1.0f, -2.0f, 0.5f));
    world.Step(kStep);
    const mir::WorldVec3 after = MetersOf(world, sphere);
    CHECK(after.x > before.x);
    CHECK(after.y < before.y);
    CHECK(world.BodyLinearVelocity(sphere).y < -2.0f);
}

TEST_CASE("A held body takes velocity changes and impulses into its kept velocities and drops forces",
          "[physics][hold]")
{
    mir::PhysicsWorldDesc weightless;
    weightless.mGravity = glm::vec3(0.0f);
    mir::PhysicsWorld world(weightless);
    const mir::BodyHandle held = world.CreateBody({.mMotion = mir::PhysicsMotion::Dynamic,
                                                   .mShape = mir::PhysicsShape::Box,
                                                   .mPosition = At(0.0, 0.0, 0.0),
                                                   .mHalfExtent = glm::vec3(0.5f, 0.25f, 0.5f)});
    const mir::BodyHandle free = world.CreateBody({.mMotion = mir::PhysicsMotion::Dynamic,
                                                   .mShape = mir::PhysicsShape::Box,
                                                   .mPosition = At(10.0, 0.0, 0.0),
                                                   .mHalfExtent = glm::vec3(0.5f, 0.25f, 0.5f)});
    world.HoldBody(held);

    for (const mir::BodyHandle body : {held, free})
    {
        world.SetBodyLinearVelocity(body, glm::vec3(2.0f, 0.0f, 0.0f));
        world.AddLinearVelocity(body, glm::vec3(0.0f, 1.0f, 0.0f));
        world.AddAngularVelocity(body, glm::vec3(0.0f, 0.5f, 0.0f));
        world.AddImpulse(body, glm::vec3(0.0f, 0.0f, 30.0f));
        world.AddAngularImpulse(body, glm::vec3(4.0f, 0.0f, 0.0f));
        world.AddImpulseAtOffset(body, glm::vec3(0.0f, 10.0f, 0.0f), glm::vec3(0.5f, 0.0f, 0.0f));
    }
    const auto close = [](const glm::vec3 &a, const glm::vec3 &b) {
        return glm::length(a - b) <= 1e-5f * std::max(1.0f, glm::length(b));
    };
    CHECK(close(world.BodyLinearVelocity(held), world.BodyLinearVelocity(free)));
    CHECK(close(world.BodyAngularVelocity(held), world.BodyAngularVelocity(free)));

    world.AddForce(held, glm::vec3(1000.0f, 0.0f, 0.0f));
    world.AddTorque(held, glm::vec3(0.0f, 1000.0f, 0.0f));
    world.AddForceAtOffset(held, glm::vec3(0.0f, 1000.0f, 0.0f), glm::vec3(0.5f, 0.0f, 0.0f));
    world.ResumeBody(held);
    world.Step(kStep);
    CHECK(close(world.BodyLinearVelocity(held), world.BodyLinearVelocity(free)));
    CHECK(close(world.BodyAngularVelocity(held), world.BodyAngularVelocity(free)));
}

TEST_CASE("Only dynamic bodies are held, and destroying a held body forgets it", "[physics][hold]")
{
    mir::PhysicsWorld world;
    const mir::BodyHandle floor = AddFloor(world);
    world.HoldBody(floor);
    CHECK_FALSE(world.IsBodyHeld(floor));
    world.HoldBody(mir::BodyHandle{});
    CHECK_FALSE(world.IsBodyHeld(mir::BodyHandle{}));

    const mir::BodyHandle sphere = world.CreateBody({.mMotion = mir::PhysicsMotion::Dynamic,
                                                     .mShape = mir::PhysicsShape::Sphere,
                                                     .mPosition = At(0.0, 3.0, 0.0),
                                                     .mRadius = 0.5f});
    world.HoldBody(sphere);
    world.HoldBody(sphere);
    world.DestroyBody(sphere);
    CHECK_FALSE(world.IsBodyHeld(sphere));
    world.ResumeBody(sphere);
    CHECK(world.BodyCount() == 1);
}

TEST_CASE("A restricted static body stops only the dynamic bodies allowed to touch it", "[physics][restrict]")
{
    mir::PhysicsWorld world;
    const mir::BodyHandle floor = AddFloor(world);
    world.RestrictBody(floor);
    const mir::BodyHandle allowed = world.CreateBody({.mMotion = mir::PhysicsMotion::Dynamic,
                                                      .mShape = mir::PhysicsShape::Sphere,
                                                      .mPosition = At(-3.0, 3.0, 0.0),
                                                      .mRadius = 0.5f});
    const mir::BodyHandle refused = world.CreateBody({.mMotion = mir::PhysicsMotion::Dynamic,
                                                      .mShape = mir::PhysicsShape::Sphere,
                                                      .mPosition = At(0.0, 3.0, 0.0),
                                                      .mRadius = 0.5f});
    const mir::BodyHandle unrestricted = world.CreateBody({.mMotion = mir::PhysicsMotion::Dynamic,
                                                           .mShape = mir::PhysicsShape::Sphere,
                                                           .mPosition = At(3.0, 3.0, 0.0),
                                                           .mRadius = 0.5f});
    world.RestrictContacts(allowed, std::span<const mir::BodyHandle>(&floor, 1));
    world.RestrictContacts(refused, {});

    StepFor(world, 120);
    CHECK(MetersOf(world, allowed).y > 0.5);
    CHECK(MetersOf(world, unrestricted).y > 0.5);
    CHECK(MetersOf(world, refused).y < -1.0);

    world.SetBodyTransform(refused, At(0.0, 3.0, 0.0), mir::kIdentityRotation);
    world.SetBodyLinearVelocity(refused, glm::vec3(0.0f));
    world.ClearContactRestriction(refused);
    StepFor(world, 120);
    CHECK(MetersOf(world, refused).y > 0.5);
}

namespace
{
    float DirectionChange(const glm::vec3 &from, const glm::vec3 &to)
    {
        const float dot = glm::dot(glm::normalize(from), glm::normalize(to));
        return glm::degrees(std::acos(std::clamp(dot, -1.0f, 1.0f)));
    }

    mir::BodyHandle AddFloatingBox(mir::PhysicsWorld &world, const mir::WorldVec3 &center, const glm::vec3 &halfExtent,
        const mir::BodyDesc &properties)
    {
        mir::BodyDesc desc = properties;
        desc.mMotion = mir::PhysicsMotion::Dynamic;
        desc.mShape = mir::PhysicsShape::Box;
        desc.mPosition = At(center);
        desc.mHalfExtent = halfExtent;
        return world.CreateBody(desc);
    }
}

TEST_CASE("Zero damping leaves a body's velocity alone and the default damping slows it", "[physics][damping]")
{
    mir::PhysicsWorld world;

    mir::BodyDesc free;
    free.mGravityFactor = 0.0f;
    free.mAllowSleeping = false;
    free.mLinearDamping = 0.0f;
    free.mAngularDamping = 0.0f;
    const mir::BodyHandle steady = AddFloatingBox(world, {}, glm::vec3(0.5f), free);

    mir::BodyDesc damped;
    damped.mGravityFactor = 0.0f;
    damped.mAllowSleeping = false;
    const mir::BodyHandle slowing = AddFloatingBox(world, mir::WorldVec3(100.0, 0.0, 0.0), glm::vec3(0.5f), damped);
    // the defaults are the ones a body had before the fields were exposed
    CHECK(world.BodyLinearDamping(slowing) == 0.05f);
    CHECK(world.BodyAngularDamping(slowing) == 0.05f);
    CHECK_FALSE(world.BodyGyroscopicForce(slowing));
    CHECK(world.BodyMotionQuality(slowing) == mir::PhysicsMotionQuality::Discrete);

    REQUIRE(world.SetBodyLinearVelocity(steady, glm::vec3(2.0f, 0.0f, 0.0f)));
    REQUIRE(world.SetBodyAngularVelocity(steady, glm::vec3(0.0f, 1.0f, 0.0f)));
    REQUIRE(world.SetBodyLinearVelocity(slowing, glm::vec3(2.0f, 0.0f, 0.0f)));
    REQUIRE(world.SetBodyAngularVelocity(slowing, glm::vec3(0.0f, 1.0f, 0.0f)));

    StepFor(world, 60);

    const glm::vec3 steadyLinear = world.BodyLinearVelocity(steady);
    const glm::vec3 steadyAngular = world.BodyAngularVelocity(steady);
    CHECK(std::abs(steadyLinear.x - 2.0f) <= 1.0e-4f * 2.0f);
    CHECK(std::abs(steadyAngular.y - 1.0f) <= 1.0e-4f);

    CHECK(world.BodyLinearVelocity(slowing).x < 2.0f * 0.99f);
    CHECK(world.BodyAngularVelocity(slowing).y < 1.0f * 0.99f);
}

TEST_CASE("The gyroscopic force turns the spin of an asymmetric body", "[physics][gyroscopic]")
{
    mir::PhysicsWorldDesc desc;
    desc.mGravity = glm::vec3(0.0f);
    mir::PhysicsWorld world(desc);

    mir::BodyDesc properties;
    properties.mAllowSleeping = false;
    properties.mLinearDamping = 0.0f;
    properties.mAngularDamping = 0.0f;
    const glm::vec3 halfExtent(0.5f, 1.0f, 2.0f);
    // off every principal axis of the box
    const glm::vec3 spin = glm::normalize(glm::vec3(1.0f, 2.0f, 3.0f)) * 4.0f;

    const mir::BodyHandle plain = AddFloatingBox(world, {}, halfExtent, properties);
    mir::BodyDesc turned = properties;
    turned.mGyroscopicForce = true;
    const mir::BodyHandle gyroscopic = AddFloatingBox(world, mir::WorldVec3(100.0, 0.0, 0.0), halfExtent, turned);

    REQUIRE(world.SetBodyAngularVelocity(plain, spin));
    REQUIRE(world.SetBodyAngularVelocity(gyroscopic, spin));
    StepFor(world, 60);

    CHECK(DirectionChange(spin, world.BodyAngularVelocity(plain)) < 0.5f);
    CHECK(DirectionChange(spin, world.BodyAngularVelocity(gyroscopic)) > 5.0f);
}

TEST_CASE("A supplied inertia tensor sets the angular response", "[physics][inertia]")
{
    mir::PhysicsWorldDesc desc;
    desc.mGravity = glm::vec3(0.0f);
    mir::PhysicsWorld world(desc);

    mir::BodyDesc properties;
    properties.mAllowSleeping = false;
    properties.mLinearDamping = 0.0f;
    properties.mAngularDamping = 0.0f;
    properties.mCustomInertia = true;
    properties.mInertiaDiagonal = glm::vec3(2.0f, 8.0f, 4.0f);

    const mir::BodyHandle custom = AddFloatingBox(world, {}, glm::vec3(0.5f), properties);
    const mir::BodyHandle shapeDerived =
        AddFloatingBox(world, mir::WorldVec3(100.0, 0.0, 0.0), glm::vec3(0.5f), mir::BodyDesc{
            .mAllowSleeping = false, .mLinearDamping = 0.0f, .mAngularDamping = 0.0f});

    const glm::vec3 torque(0.0f, 16.0f, 0.0f);
    world.AddTorque(custom, torque);
    world.AddTorque(shapeDerived, torque);
    world.Step(kStep);

    const float expected = torque.y / 8.0f * kStep;
    const float customY = world.BodyAngularVelocity(custom).y;
    const float shapeDerivedY = world.BodyAngularVelocity(shapeDerived).y;
    CHECK(std::abs(customY - expected) <= 1.0e-3f * std::abs(expected));
    CHECK(std::abs(shapeDerivedY - expected) > 1.0e-3f);
}

TEST_CASE("Force and impulse offsets are measured from the centre of mass", "[physics][inertia][centerOfMass]")
{
    mir::PhysicsWorldDesc desc;
    desc.mGravity = glm::vec3(0.0f);
    mir::PhysicsWorld world(desc);

    mir::BodyDesc properties;
    properties.mAllowSleeping = false;
    properties.mLinearDamping = 0.0f;
    properties.mAngularDamping = 0.0f;
    properties.mCustomInertia = true;
    properties.mInertiaDiagonal = glm::vec3(2.0f, 8.0f, 4.0f);
    properties.mCustomCenterOfMass = true;
    properties.mCenterOfMassOffset = glm::vec3(1.0f, 0.0f, 0.0f);
    const mir::BodyHandle body = AddFloatingBox(world, {}, glm::vec3(0.5f), properties);
    world.Step(kStep);
    CHECK(MetersOf(world, body) == mir::WorldVec3(0.0));

    const glm::vec3 force(0.0f, 10.0f, 0.0f);
    world.AddForceAtOffset(body, force, glm::vec3(0.0f));
    world.Step(kStep);
    CHECK(glm::length(world.BodyAngularVelocity(body)) <= 1.0e-6f);

    world.SetBodyAngularVelocity(body, glm::vec3(0.0f));
    world.AddForceAtOffset(body, force, glm::vec3(-1.0f, 0.0f, 0.0f));
    world.Step(kStep);
    const float expectedForce = -10.0f / 4.0f * kStep;
    CHECK(std::abs(world.BodyAngularVelocity(body).z - expectedForce) <= 1.0e-3f * std::abs(expectedForce));

    world.SetBodyAngularVelocity(body, glm::vec3(0.0f));
    world.AddImpulseAtOffset(body, force, glm::vec3(0.0f));
    CHECK(glm::length(world.BodyAngularVelocity(body)) <= 1.0e-6f);

    world.AddImpulseAtOffset(body, force, glm::vec3(-1.0f, 0.0f, 0.0f));
    const float expectedImpulse = -10.0f / 4.0f;
    CHECK(std::abs(world.BodyAngularVelocity(body).z - expectedImpulse) <= 1.0e-3f * std::abs(expectedImpulse));
}

TEST_CASE("A supplied inertia tensor survives a shape rebuild and a mass change", "[physics][inertia]")
{
    mir::PhysicsWorldDesc desc;
    desc.mGravity = glm::vec3(0.0f);
    mir::PhysicsWorld world(desc);

    mir::BodyDesc properties;
    properties.mAllowSleeping = false;
    properties.mLinearDamping = 0.0f;
    properties.mAngularDamping = 0.0f;
    properties.mCustomInertia = true;
    properties.mInertiaDiagonal = glm::vec3(2.0f, 8.0f, 4.0f);
    const mir::BodyHandle body = AddFloatingBox(world, {}, glm::vec3(0.5f), properties);

    const auto response = [&]
    {
        world.SetBodyTransform(body, At(0.0, 0.0, 0.0), mir::kIdentityRotation);
        world.SetBodyLinearVelocity(body, glm::vec3(0.0f));
        world.SetBodyAngularVelocity(body, glm::vec3(0.0f));
        world.AddTorque(body, glm::vec3(0.0f, 16.0f, 0.0f));
        world.Step(kStep);
        return world.BodyAngularVelocity(body).y;
    };
    const float expected = 16.0f / 8.0f * kStep;
    const float original = response();
    REQUIRE(std::abs(original - expected) <= 1.0e-3f * expected);

    mir::BodyDesc rebuilt = properties;
    rebuilt.mHalfExtent = glm::vec3(1.0f, 2.0f, 3.0f);
    world.SetBodyShape(body, rebuilt);
    const float afterShape = response();

    world.SetBodyMass(body, 50.0f);
    const float afterMass = response();

    CHECK(std::abs(afterShape - original) <= 1.0e-3f * std::abs(original));
    CHECK(std::abs(afterMass - original) <= 1.0e-3f * std::abs(original));

    world.SetBodyLinearVelocity(body, glm::vec3(0.0f));
    world.AddImpulse(body, glm::vec3(0.0f, 50.0f, 0.0f));
    CHECK(std::abs(world.BodyLinearVelocity(body).y - 1.0f) <= 1.0e-3f);
}

TEST_CASE("An invalid inertia tensor falls back to the shape's and is logged", "[physics][inertia]")
{
    mir::PhysicsWorldDesc desc;
    desc.mGravity = glm::vec3(0.0f);
    mir::PhysicsWorld world(desc);

    mir::BodyDesc properties;
    properties.mAllowSleeping = false;
    properties.mLinearDamping = 0.0f;
    properties.mAngularDamping = 0.0f;
    properties.mCustomInertia = true;

    const mir::BodyHandle shapeDerived =
        AddFloatingBox(world, {}, glm::vec3(0.5f), mir::BodyDesc{
            .mAllowSleeping = false, .mLinearDamping = 0.0f, .mAngularDamping = 0.0f});

    mir::InitLog({ .filePath = (std::filesystem::temp_directory_path() / "mir_physics_inertia.log").string() });
    mir::LogHistory::Clear();
    mir::BodyDesc nonPositive = properties;
    nonPositive.mInertiaDiagonal = glm::vec3(0.0f, 8.0f, 4.0f);
    const mir::BodyHandle refusedDiagonal =
        AddFloatingBox(world, mir::WorldVec3(100.0, 0.0, 0.0), glm::vec3(0.5f), nonPositive);

    mir::BodyDesc nonDefinite = properties;
    nonDefinite.mInertiaDiagonal = glm::vec3(1.0f, 1.0f, 1.0f);
    nonDefinite.mInertiaProducts = glm::vec3(2.0f, 0.0f, 0.0f);
    const mir::BodyHandle refusedProducts =
        AddFloatingBox(world, mir::WorldVec3(200.0, 0.0, 0.0), glm::vec3(0.5f), nonDefinite);

    bool logged = false;
    for (const mir::LogEntry &entry : mir::LogHistory::Snapshot())
        logged = logged || entry.message.find("inertia tensor") != std::string::npos;
    CHECK(logged);

    for (const mir::BodyHandle body : {shapeDerived, refusedDiagonal, refusedProducts})
        world.AddTorque(body, glm::vec3(0.0f, 16.0f, 0.0f));
    world.Step(kStep);

    const float expected = world.BodyAngularVelocity(shapeDerived).y;
    CHECK(world.BodyAngularVelocity(refusedDiagonal).y == Catch::Approx(expected));
    CHECK(world.BodyAngularVelocity(refusedProducts).y == Catch::Approx(expected));
}

TEST_CASE("A custom centre of mass is measured from the body origin whatever the collider offset",
          "[physics][inertia][centerOfMass]")
{
    mir::PhysicsWorldDesc desc;
    desc.mGravity = glm::vec3(0.0f);
    mir::PhysicsWorld world(desc);

    mir::BodyDesc properties;
    properties.mAllowSleeping = false;
    properties.mLinearDamping = 0.0f;
    properties.mAngularDamping = 0.0f;
    properties.mCustomInertia = true;
    properties.mInertiaDiagonal = glm::vec3(2.0f);
    properties.mCustomCenterOfMass = true;
    properties.mOffset = glm::vec3(1.0f, 0.0f, 0.0f);

    SECTION("at the origin")
    {
        properties.mCenterOfMassOffset = glm::vec3(0.0f);
    }
    SECTION("away from the origin and the collider")
    {
        properties.mCenterOfMassOffset = glm::vec3(0.25f, 0.5f, 0.0f);
    }
    SECTION("at the collider")
    {
        properties.mCenterOfMassOffset = properties.mOffset;
    }

    const mir::BodyHandle body = AddFloatingBox(world, {}, glm::vec3(0.5f), properties);
    world.SetBodyAngularVelocity(body, glm::vec3(0.0f, 0.0f, 2.0f));
    for (int step = 0; step < 30; ++step)
        world.Step(kStep);

    // A free body spins about its centre of mass, so that point stays where it was.
    const glm::vec3 center = properties.mCenterOfMassOffset;
    const mir::WorldVec3 moved = MetersOf(world, body) + mir::WorldVec3(world.BodyRotation(body) * center);
    CHECK(glm::length(glm::vec3(moved - mir::WorldVec3(center))) <= 1.0e-3f);
}
