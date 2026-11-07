/**
 * @file ActiveCamera.tests.cpp
 * @author Quy Dong Ta
 * @brief Tests for renderer/ActiveCamera: finding and setting the active
 *        Camera, the camera Play falls back to, and ActiveCameraSystem
 *        keeping one flagged.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer/ActiveCamera.h>

#include <core/ecs/CommandBuffer.h>
#include <core/ecs/SystemScheduler.h>
#include <core/ecs/TransformHierarchy.h>
#include <core/ecs/World.h>

#include <catch2/catch_test_macros.hpp>

namespace
{
    using mir::Camera;
    using mir::Entity;
    using mir::World;

    Entity MakeCamera(World &world, bool active)
    {
        const Entity entity = world.CreateEntity();
        mir::AddTransform(world, entity);
        Camera camera{};
        camera.mActive = active;
        world.AddComponent<Camera>(entity, camera);
        return entity;
    }

    bool IsActive(World &world, Entity camera) { return world.GetComponent<Camera>(camera)->mActive; }

    struct SystemFixture
    {
        World world;
        mir::CommandBuffer commands;
        mir::SystemScheduler scheduler;
        mir::SystemContext context{ world, commands, 0.016f, 0.0, 0 };
        mir::ActiveCameraSystem *system = nullptr;

        SystemFixture()
        {
            system = &scheduler.AddSystem<mir::ActiveCameraSystem>(mir::SystemPhase::Render,
                mir::SystemOrder{ mir::SystemOrder::kEarly });
            scheduler.Start(context);
        }

        ~SystemFixture()
        {
            if (scheduler.Started())
                scheduler.Stop(context);
        }

        void Update() { scheduler.Update(context); }
    };
}

TEST_CASE("FindActiveCamera returns the flagged camera, or null", "[renderer][camera]")
{
    World world;
    MakeCamera(world, false);
    CHECK(mir::FindActiveCamera(world).IsNull());

    const Entity active = MakeCamera(world, true);
    CHECK(mir::FindActiveCamera(world) == active);
}

TEST_CASE("Additive cameras retain their flags and follow primary scene selection", "[renderer][camera][additive]")
{
    SystemFixture fixture;
    const auto first = mir::AllocateSceneInstanceId();
    const auto second = mir::AllocateSceneInstanceId();
    fixture.world.SetPrimaryScene(first);
    const Entity a = MakeCamera(fixture.world, true);
    Entity b;
    {
        const World::SceneScope scope(fixture.world, second);
        b = MakeCamera(fixture.world, true);
    }
    fixture.Update();
    CHECK(IsActive(fixture.world, a));
    CHECK(IsActive(fixture.world, b));
    CHECK(mir::ResolvePlayCamera(fixture.world).mEntity == a);
    CHECK(mir::FindActiveCamera(fixture.world) == a);
    fixture.world.SetPrimaryScene(second);
    CHECK(mir::ResolvePlayCamera(fixture.world).mEntity == b);
    CHECK(mir::FindActiveCamera(fixture.world) == b);
    REQUIRE(mir::SetActiveCamera(fixture.world, b));
    CHECK(IsActive(fixture.world, a));
    fixture.world.DestroyEntity(b);
    CHECK(mir::ResolvePlayCamera(fixture.world).mEntity == a);
}

TEST_CASE("SetActiveCamera flags one camera and clears the rest", "[renderer][camera]")
{
    World world;
    const Entity a = MakeCamera(world, true);
    const Entity b = MakeCamera(world, false);
    const Entity c = MakeCamera(world, true);

    REQUIRE(mir::SetActiveCamera(world, b));
    CHECK_FALSE(IsActive(world, a));
    CHECK(IsActive(world, b));
    CHECK_FALSE(IsActive(world, c));
}

TEST_CASE("SetActiveCamera on an entity without a Camera changes nothing", "[renderer][camera]")
{
    World world;
    const Entity camera = MakeCamera(world, true);
    const Entity bare = world.CreateEntity();

    CHECK_FALSE(mir::SetActiveCamera(world, bare));
    CHECK(IsActive(world, camera));
}

TEST_CASE("Play draws through the flagged camera, not the first one", "[renderer][camera]")
{
    World world;
    MakeCamera(world, false);
    const Entity active = MakeCamera(world, true);

    const mir::PlayCamera play = mir::ResolvePlayCamera(world);
    CHECK(play.mEntity == active);
    CHECK(play.mActive);
}

TEST_CASE("With no camera flagged, Play falls back to the lowest-index one", "[renderer][camera]")
{
    World world;
    const Entity first = MakeCamera(world, false);
    MakeCamera(world, false);

    const mir::PlayCamera play = mir::ResolvePlayCamera(world);
    CHECK(play.mEntity == first);
    CHECK_FALSE(play.mActive);
}

TEST_CASE("With no camera at all, Play has none to draw through", "[renderer][camera]")
{
    World world;
    const mir::PlayCamera play = mir::ResolvePlayCamera(world);
    CHECK(play.mEntity.IsNull());
    CHECK_FALSE(play.mActive);
}

TEST_CASE("Flagging a second camera clears the one that was active", "[renderer][camera]")
{
    SystemFixture fixture;
    const Entity a = MakeCamera(fixture.world, true);
    const Entity b = MakeCamera(fixture.world, false);
    fixture.Update();

    fixture.world.GetComponent<Camera>(b)->mActive = true;
    fixture.Update();

    CHECK_FALSE(IsActive(fixture.world, a));
    CHECK(IsActive(fixture.world, b));
}

TEST_CASE("A camera that arrives flagged loses to the active one", "[renderer][camera]")
{
    SystemFixture fixture;
    const Entity original = MakeCamera(fixture.world, true);
    fixture.Update();

    const Entity duplicate = MakeCamera(fixture.world, true);
    fixture.Update();

    CHECK(IsActive(fixture.world, original));
    CHECK_FALSE(IsActive(fixture.world, duplicate));
}

TEST_CASE("Cameras that all arrive flagged keep the lowest-index one", "[renderer][camera]")
{
    SystemFixture fixture;
    const Entity first = MakeCamera(fixture.world, true);
    const Entity second = MakeCamera(fixture.world, true);
    fixture.Update();

    CHECK(IsActive(fixture.world, first));
    CHECK_FALSE(IsActive(fixture.world, second));
}

TEST_CASE("Deleting the active camera promotes no other", "[renderer][camera]")
{
    SystemFixture fixture;
    const Entity active = MakeCamera(fixture.world, true);
    const Entity other = MakeCamera(fixture.world, false);
    fixture.Update();

    fixture.world.DestroyEntity(active);
    fixture.Update();

    CHECK_FALSE(IsActive(fixture.world, other));
    CHECK(mir::ResolvePlayCamera(fixture.world).mEntity == other);
}

TEST_CASE("After a reset every flagged camera counts as arriving", "[renderer][camera]")
{
    SystemFixture fixture;
    const Entity a = MakeCamera(fixture.world, false);
    const Entity b = MakeCamera(fixture.world, true);
    fixture.Update();

    // what a scene load can leave: two flagged, neither newer than the other
    fixture.world.GetComponent<Camera>(a)->mActive = true;
    fixture.system->Reset();
    fixture.Update();

    CHECK(IsActive(fixture.world, a));
    CHECK_FALSE(IsActive(fixture.world, b));
}
