/**
 * @file ColliderLines.tests.cpp
 * @author Rahul Nair
 * @brief Tests for physics/ColliderLines, PhysicsWorld::ForEachCollider and ColliderDebugSystem.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <physics/ColliderLines.h>

#include <physics/ColliderDebugSystem.h>
#include <physics/PhysicsSystem.h>
#include <physics/PhysicsWorld.h>
#include <physics/components/RigidBody.h>

#include <core/debug/DebugLines.h>
#include <core/ecs/CommandBuffer.h>
#include <core/ecs/EngineMode.h>
#include <core/ecs/SystemScheduler.h>
#include <core/ecs/TransformHierarchy.h>
#include <core/ecs/World.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <vector>

using namespace mir;

namespace
{
    constexpr float kStep = 1.0f / 60.0f;
    constexpr double kTolerance = 1.0e-4;

    WorldPosition At(const glm::dvec3 &meters)
    {
        const std::optional<WorldPosition> position = WorldPosition::FromDoubleMeters(meters);
        REQUIRE(position);
        return *position;
    }

    glm::dvec3 Meters(WorldPosition position) { return glm::dvec3(position.Meters()); }

    std::vector<DebugLineVertex> LinesOf(const ColliderView &view)
    {
        std::vector<DebugLineVertex> out;
        AppendColliderLines(view, out);
        return out;
    }

    // Every point of out, in view's own frame.
    std::vector<glm::dvec3> LocalPoints(const ColliderView &view, const std::vector<DebugLineVertex> &out)
    {
        std::vector<glm::dvec3> points;
        const glm::dquat inverse = glm::inverse(glm::dquat(view.mRotation));
        for (const DebugLineVertex &vertex : out)
            points.push_back(inverse * (Meters(vertex.position) - Meters(view.mPosition)));
        return points;
    }

    std::vector<ColliderView> CollidersOf(const PhysicsWorld &physics)
    {
        std::vector<ColliderView> views;
        physics.ForEachCollider([&](const ColliderView &view) { views.push_back(view); });
        return views;
    }

    // What PhysicsModule wires up for colliders, plus the renderer's DebugLines.
    struct ColliderFixture
    {
        World world;
        CommandBuffer commands;
        SystemScheduler scheduler;
        SystemContext context{ world, commands, kStep, 0.0, 0 };
        Registration hooks;

        explicit ColliderFixture(EngineMode mode)
        {
            world.EmplaceResource<PhysicsWorld>();
            world.EmplaceResource<DebugLines>().mCollidersWanted = true;
            hooks = world.SetComponentHooks<RigidBody>(
                { .onAdd = &PhysicsSystem::OnRigidBodyAdded, .onRemove = &PhysicsSystem::OnRigidBodyRemoved });
            world.EmplaceResource<EngineModeState>();
            SetEngineMode(world, mode);
            SetPlayState(world, PlayState::Running);
            world.Resource<PhysicsWorld>().SetSimulationLive(mode == EngineMode::Play);

            scheduler.AddSystem<PhysicsSystem>(SystemPhase::FixedUpdate, SystemOrder{ SystemOrder::kNormal });
            scheduler.AddSystem<ColliderDebugSystem>(SystemPhase::LateUpdate, SystemOrder{ SystemOrder::kNormal });
            scheduler.Start(context);
        }

        ~ColliderFixture()
        {
            if (scheduler.Started())
                scheduler.Stop(context);
        }

        void Run() { scheduler.Update(context, 1, kStep); }

        Entity Add(const WorldVec3 &position, const RigidBody &rigidBody)
        {
            const Entity entity = world.CreateEntity();
            AddTransform(world, entity, Transform{ position });
            world.AddComponent<RigidBody>(entity, rigidBody);
            return entity;
        }

        const std::vector<DebugLineVertex> &Lines() { return world.Resource<DebugLines>().mColliders; }
    };
}

TEST_CASE("A box outlines its 12 edges at its half extents", "[physics][collider-lines]")
{
    ColliderView view;
    view.mShape = PhysicsShape::Box;
    view.mPosition = At({ 10.0, -2.0, 3.5 });
    view.mRotation = glm::angleAxis(0.7f, glm::normalize(glm::vec3(1.0f, 2.0f, 0.5f)));
    view.mHalfExtent = { 1.0f, 2.0f, 0.5f };

    const std::vector<DebugLineVertex> lines = LinesOf(view);
    REQUIRE(lines.size() == 24);
    for (const glm::dvec3 &point : LocalPoints(view, lines))
    {
        CHECK(std::abs(std::abs(point.x) - 1.0) < kTolerance);
        CHECK(std::abs(std::abs(point.y) - 2.0) < kTolerance);
        CHECK(std::abs(std::abs(point.z) - 0.5) < kTolerance);
    }
}

TEST_CASE("A sphere outlines three circles at its radius", "[physics][collider-lines]")
{
    ColliderView view;
    view.mShape = PhysicsShape::Sphere;
    view.mPosition = At({ -4.0, 1.0, 0.0 });
    view.mRadius = 1.5f;

    const std::vector<DebugLineVertex> lines = LinesOf(view);
    CHECK(lines.size() == 3 * 2 * kColliderCircleSegments);
    for (const glm::dvec3 &point : LocalPoints(view, lines))
        CHECK(std::abs(glm::length(point) - 1.5) < kTolerance);
}

TEST_CASE("A capsule stands along its local Y, caps included", "[physics][collider-lines]")
{
    ColliderView view;
    view.mShape = PhysicsShape::Capsule;
    view.mRotation = glm::angleAxis(glm::half_pi<float>(), glm::vec3(0.0f, 0.0f, 1.0f));
    view.mRadius = 0.5f;
    view.mCylinderHalfHeight = 1.0f;

    double highest = 0.0;
    for (const glm::dvec3 &point : LocalPoints(view, LinesOf(view)))
    {
        // every point lies on the capsule's surface: radius away from the segment between the cap centers
        const double along = std::clamp(point.y, -1.0, 1.0);
        CHECK(std::abs(glm::length(point - glm::dvec3(0.0, along, 0.0)) - 0.5) < kTolerance);
        highest = std::max(highest, std::abs(point.y));
    }
    CHECK(std::abs(highest - 1.5) < kTolerance);
}

TEST_CASE("A height field skips the edges that touch a hole", "[physics][collider-lines]")
{
    const float hole = std::numeric_limits<float>::quiet_NaN();
    // 3 x 3, the center a hole: of its 12 edges only the 8 around the rim stay
    const std::vector<glm::vec3> grid = {
        { 0, 0, 0 },{ 1, 0, 0 },{ 2, 0, 0 },
        { 0, 0, 1 },{ 1, hole, 1 },{ 2, 0, 1 },
        { 0, 0, 2 },{ 1, 0, 2 },{ 2, 0, 2 } };

    ColliderView view;
    view.mHeightField = true;
    view.mHeightGrid = grid;
    view.mGridSize = 3;

    CHECK(LinesOf(view).size() == 2 * 8);
}

TEST_CASE("A collider's color says trigger, then sleeping, then motion", "[physics][collider-lines]")
{
    ColliderView dynamic;
    dynamic.mMotion = PhysicsMotion::Dynamic;
    ColliderView fixed;
    fixed.mMotion = PhysicsMotion::Static;
    ColliderView kinematic;
    kinematic.mMotion = PhysicsMotion::Kinematic;

    CHECK(ColliderColor(dynamic) != ColliderColor(fixed));
    CHECK(ColliderColor(kinematic) == ColliderColor(fixed));

    ColliderView sleeping = dynamic;
    sleeping.mSleeping = true;
    CHECK(ColliderColor(sleeping) != ColliderColor(dynamic));

    ColliderView trigger = sleeping;
    trigger.mTrigger = true;
    CHECK(ColliderColor(trigger) != ColliderColor(sleeping));
    CHECK(ColliderColor(trigger) != ColliderColor(dynamic));
}

TEST_CASE("ColliderViewOf places the shape as CreateBody builds it", "[physics][collider-lines]")
{
    BodyDesc desc;
    desc.mShape = PhysicsShape::Capsule;
    desc.mAxis = PhysicsAxis::X;
    desc.mRadius = 0.25f;
    desc.mHalfHeight = 1.0f;
    desc.mPosition = At({ 5.0, 0.0, 0.0 });
    desc.mRotation = glm::angleAxis(glm::half_pi<float>(), glm::vec3(0.0f, 1.0f, 0.0f));
    desc.mOffset = { 1.0f, 0.0f, 0.0f };
    desc.mIsTrigger = true;

    const std::optional<ColliderView> view = ColliderViewOf(desc);
    REQUIRE(view);
    // the offset is in the body's frame: a quarter turn about Y takes +X to -Z
    const glm::dvec3 center = Meters(view->mPosition);
    CHECK(std::abs(center.x - 5.0) < kTolerance);
    CHECK(std::abs(center.z + 1.0) < kTolerance);
    // a capsule along X stands its local Y on the body's X, which the quarter turn takes to -Z
    const glm::vec3 up = view->mRotation * glm::vec3(0.0f, 1.0f, 0.0f);
    CHECK(std::abs(std::abs(up.z) - 1.0f) < 1.0e-4f);
    CHECK(view->mCylinderHalfHeight == Catch::Approx(0.75f));
    CHECK(view->mTrigger);

    desc.mRadius = 0.0f;
    CHECK_FALSE(ColliderViewOf(desc).has_value());
}

TEST_CASE("ForEachCollider reports each body's shape, offset and state", "[physics][collider-lines]")
{
    PhysicsWorld physics;

    BodyDesc box;
    box.mMotion = PhysicsMotion::Dynamic;
    box.mShape = PhysicsShape::Box;
    box.mHalfExtent = { 1.0f, 0.5f, 2.0f };
    box.mPosition = At({ 0.0, 10.0, 0.0 });
    box.mOffset = { 0.0f, 1.0f, 0.0f };
    physics.CreateBody(box);

    BodyDesc trigger;
    trigger.mMotion = PhysicsMotion::Static;
    trigger.mShape = PhysicsShape::Sphere;
    trigger.mRadius = 3.0f;
    trigger.mPosition = At({ 20.0, 0.0, 0.0 });
    trigger.mIsTrigger = true;
    physics.CreateBody(trigger);

    const std::vector<ColliderView> views = CollidersOf(physics);
    REQUIRE(views.size() == 2);
    const ColliderView &first = views[0].mShape == PhysicsShape::Box ? views[0] : views[1];
    const ColliderView &second = views[0].mShape == PhysicsShape::Box ? views[1] : views[0];

    CHECK(first.mMotion == PhysicsMotion::Dynamic);
    CHECK(glm::length(first.mHalfExtent - glm::vec3(1.0f, 0.5f, 2.0f)) < 1.0e-4f);
    CHECK(std::abs(Meters(first.mPosition).y - 11.0) < kTolerance);
    CHECK_FALSE(first.mTrigger);

    CHECK(second.mShape == PhysicsShape::Sphere);
    CHECK(second.mRadius == Catch::Approx(3.0f));
    CHECK(second.mTrigger);
    CHECK_FALSE(second.mSleeping);
}

TEST_CASE("ForEachCollider follows a falling body's simulated pose", "[physics][collider-lines]")
{
    PhysicsWorld physics;
    physics.SetSimulationLive(true);

    BodyDesc sphere;
    sphere.mShape = PhysicsShape::Sphere;
    sphere.mPosition = At({ 0.0, 10.0, 0.0 });
    const BodyHandle body = physics.CreateBody(sphere);

    for (int i = 0; i < 30; ++i)
        physics.Step(kStep);

    const std::vector<ColliderView> views = CollidersOf(physics);
    REQUIRE(views.size() == 1);
    const std::optional<WorldPosition> simulated = physics.BodyPosition(body);
    REQUIRE(simulated);
    CHECK(Meters(views[0].mPosition).y < 10.0);
    CHECK(std::abs(Meters(views[0].mPosition).y - Meters(*simulated).y) < kTolerance);
}

TEST_CASE("ForEachCollider thins a height field to a grid and marks its holes", "[physics][collider-lines]")
{
    PhysicsWorld physics;

    std::vector<float> samples(5 * 5, 2.0f);
    samples[12] = kHeightFieldHole;
    HeightFieldDesc desc;
    desc.mSamples = samples;
    desc.mCountX = 5;
    desc.mCountZ = 5;
    desc.mSpacing = 1.0f;
    desc.mOrigin = At({ 100.0, 0.0, 100.0 });
    REQUIRE(physics.CreateStaticHeightField(desc).IsValid());

    // the grid lives only for the call, so it is read inside it
    int fields = 0;
    physics.ForEachCollider([&](const ColliderView &field)
        {
            ++fields;
            REQUIRE(field.mHeightField);
            REQUIRE(field.mGridSize >= 5);
            REQUIRE(field.mHeightGrid.size() == static_cast<size_t>(field.mGridSize) * field.mGridSize);

            CHECK(field.mHeightGrid[0].y == Catch::Approx(2.0f).margin(0.01f));
            CHECK(std::isnan(field.mHeightGrid[2 * field.mGridSize + 2].y));
            CHECK(std::abs(Meters(field.mPosition).x - 100.0) < kTolerance);
        });
    CHECK(fields == 1);
}

TEST_CASE("Edit outlines the bodies PlayBegin will build", "[physics][collider-lines]")
{
    ColliderFixture fixture(EngineMode::Edit);
    fixture.Add({ 0.0, 2.0, 0.0 }, RigidBody{ .shape = PhysicsShape::Box });
    fixture.Run();

    CHECK(fixture.Lines().size() == 24);
    CHECK(fixture.world.Resource<PhysicsWorld>().BodyCount() == 0);
}

TEST_CASE("Play outlines the live bodies once each", "[physics][collider-lines]")
{
    ColliderFixture fixture(EngineMode::Play);
    fixture.Add({ 0.0, 2.0, 0.0 }, RigidBody{ .shape = PhysicsShape::Box });
    fixture.Run();

    CHECK(fixture.world.Resource<PhysicsWorld>().BodyCount() == 1);
    CHECK(fixture.Lines().size() == 24);
}

TEST_CASE("No lines are built while no view wants them", "[physics][collider-lines]")
{
    ColliderFixture fixture(EngineMode::Edit);
    fixture.Add({ 0.0, 2.0, 0.0 }, RigidBody{ .shape = PhysicsShape::Box });
    fixture.Run();
    REQUIRE_FALSE(fixture.Lines().empty());

    fixture.world.Resource<DebugLines>().mCollidersWanted = false;
    fixture.Run();
    CHECK(fixture.Lines().empty());
}
