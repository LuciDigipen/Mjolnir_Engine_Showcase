/**
 * @file CameraCut.tests.cpp
 * @author Quy Dong Ta
 * @brief Tests for renderer/CameraCut: marking a camera cut, and a cut lasting
 *        until the frame that consumes it.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer/CameraCut.h>

#include <core/ecs/World.h>
#include <renderer/components/Camera.h>

#include <catch2/catch_test_macros.hpp>

namespace
{
    using mir::Camera;
    using mir::Entity;
    using mir::World;

    Entity MakeCamera(World &world)
    {
        const Entity entity = world.CreateEntity();
        world.AddComponent<Camera>(entity, Camera{});
        return entity;
    }
}

TEST_CASE("A cut camera stays cut until the cuts are cleared", "[renderer][camera]")
{
    World world;
    const Entity camera = MakeCamera(world);
    CHECK_FALSE(mir::IsCameraCut(world, camera));

    REQUIRE(mir::CutCamera(world, camera));
    CHECK(mir::IsCameraCut(world, camera));
    CHECK(mir::IsCameraCut(world, camera));

    mir::ClearCameraCuts(world);
    CHECK_FALSE(mir::IsCameraCut(world, camera));
}

TEST_CASE("A cut belongs to the camera that was cut", "[renderer][camera]")
{
    World world;
    const Entity cut = MakeCamera(world);
    const Entity other = MakeCamera(world);

    REQUIRE(mir::CutCamera(world, cut));
    CHECK_FALSE(mir::IsCameraCut(world, other));
    CHECK_FALSE(mir::IsCameraCut(world, mir::kNullEntity));
}

TEST_CASE("Cutting a camera twice before a frame records one cut", "[renderer][camera]")
{
    World world;
    const Entity camera = MakeCamera(world);

    REQUIRE(mir::CutCamera(world, camera));
    REQUIRE(mir::CutCamera(world, camera));
    CHECK(world.Resource<mir::CameraCuts>().mCameras.size() == 1);
}

TEST_CASE("CutCamera on an entity without a Camera marks nothing", "[renderer][camera]")
{
    World world;
    const Entity bare = world.CreateEntity();

    CHECK_FALSE(mir::CutCamera(world, bare));
    CHECK_FALSE(mir::IsCameraCut(world, bare));
    CHECK_FALSE(mir::CutCamera(world, mir::kNullEntity));
}
