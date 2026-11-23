/**
 * @file RenderSystem.tests.cpp
 * @author Jackey Cen
 * @brief Tests for renderer/RenderSystem.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer/RenderSystem.h>

#include <core/ecs/TransformHierarchy.h>
#include <core/ecs/World.h>

#include <catch2/catch_test_macros.hpp>

namespace
{
    mir::Entity MakeCamera(mir::World &world, mir::WorldVec3 position, float fov, bool active)
    {
        const mir::Entity entity = world.CreateEntity();
        mir::AddTransform(world, entity, mir::Transform(position));
        mir::ResolveWorld(world, entity);
        mir::Camera camera{};
        camera.mFovYDegrees = fov;
        camera.mActive = active;
        world.AddComponent<mir::Camera>(entity, camera);
        return entity;
    }

    constexpr VkExtent2D kSwapchain{ 1920, 1080 };
}

TEST_CASE("A source World without a Sun or a Sky gets default lighting", "[renderer]")
{
    const mir::SourceLighting lighting = mir::ResolveSourceLighting(false, false, true);
    CHECK(lighting.mUseDefaultSun);
    CHECK(lighting.mUseDefaultSky);
}

TEST_CASE("Source lighting defaults never replace authored lights", "[renderer]")
{
    const mir::SourceLighting sun = mir::ResolveSourceLighting(true, false, true);
    CHECK_FALSE(sun.mUseDefaultSun);
    CHECK_FALSE(sun.mUseDefaultSky);

    const mir::SourceLighting sky = mir::ResolveSourceLighting(false, true, true);
    CHECK_FALSE(sky.mUseDefaultSun);
    CHECK_FALSE(sky.mUseDefaultSky);
}

TEST_CASE("A viewport draws through the Play camera at its rect's aspect, which the primary view carries",
    "[renderer][view]")
{
    mir::World world;
    MakeCamera(world, mir::WorldVec3(0), 60.0f, false);
    MakeCamera(world, mir::WorldVec3(1, 2, 3), 45.0f, true);
    const mir::Viewport viewport{ .mRect = VkRect2D{ { 10, 20 }, { 800, 400 } } };

    const auto view = mir::ResolveViewportCamera(viewport, 0, world, mir::ResolvePlayCamera(world), kSwapchain);
    REQUIRE(view.has_value());
    CHECK(view->pose.position.Meters() == mir::WorldVec3(1, 2, 3));
    CHECK(view->lens.mFovYDegrees == 45.0f);
    CHECK(view->extent.width == 800);
    CHECK(view->extent.height == 400);
    CHECK(view->viewProj == mir::MakeCameraRelativeViewProjection(view->pose, view->lens, 2.0f));

    const mir::PrimaryView primary = mir::MakePrimaryView(*view);
    CHECK(primary.mValid);
    CHECK(primary.mCameraPosition.Meters() == mir::WorldVec3(1, 2, 3));
    CHECK(primary.mViewProjection == view->viewProj);
    CHECK(primary.mWidth == 800);
    CHECK(primary.mHeight == 400);
}

TEST_CASE("An empty rect is the swapchain, and with no Camera the view is the origin with a default lens",
    "[renderer][view]")
{
    mir::World world;
    const auto view = mir::ResolveViewportCamera(mir::Viewport{}, 0, world, mir::ResolvePlayCamera(world), kSwapchain);
    REQUIRE(view.has_value());
    CHECK(view->pose.position.Meters() == mir::WorldVec3(0));
    CHECK(view->lens.mFovYDegrees == mir::Camera{}.mFovYDegrees);
    CHECK(view->extent.width == 1920);
    CHECK(view->viewProj == mir::MakeCameraRelativeViewProjection(view->pose, view->lens, 1920.0f / 1080.0f));
}

TEST_CASE("An external camera wins, and a viewport whose Camera entity is gone has no view", "[renderer][view]")
{
    mir::World world;
    MakeCamera(world, mir::WorldVec3(1, 2, 3), 45.0f, true);

    mir::Viewport external;
    external.mExternalCameraPose.position = *mir::WorldPosition::FromMeters(mir::WorldVec3(7, 8, 9));
    external.mUseExternalCamera = true;
    const auto editor = mir::ResolveViewportCamera(external, 0, world, mir::ResolvePlayCamera(world), kSwapchain);
    REQUIRE(editor.has_value());
    CHECK(editor->pose.position.Meters() == mir::WorldVec3(7, 8, 9));

    const mir::Entity gone = MakeCamera(world, mir::WorldVec3(4, 5, 6), 30.0f, false);
    world.DestroyEntity(gone);
    mir::Viewport chosen;
    chosen.mCameraEntity = gone;
    CHECK_FALSE(mir::ResolveViewportCamera(chosen, 1, world, mir::ResolvePlayCamera(world), kSwapchain).has_value());
}

TEST_CASE("The main World keeps its own lighting", "[renderer]")
{
    const mir::SourceLighting lighting = mir::ResolveSourceLighting(false, false, false);
    CHECK_FALSE(lighting.mUseDefaultSun);
    CHECK_FALSE(lighting.mUseDefaultSky);
}
