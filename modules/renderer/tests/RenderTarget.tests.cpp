/**
 * @file RenderTarget.tests.cpp
 * @author Quy Dong Ta
 * @brief Tests for how a frame's views share the view slots between the screen and render targets.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer/RenderTargetCameras.h>
#include <renderer/VulkanRenderer.h>

#include <core/ecs/TransformHierarchy.h>
#include <core/ecs/World.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <optional>
#include <utility>
#include <vector>

namespace
{
    // fovYDegrees tells the views apart
    mir::RenderView Screen(float tag)
    {
        mir::RenderView view;
        view.fovYDegrees = tag;
        return view;
    }

    mir::RenderView Target(float tag, uint32_t index)
    {
        mir::RenderView view = Screen(tag);
        view.target = mir::RenderTargetHandle{ .mIndex = index };
        return view;
    }

    std::vector<float> Tags(const std::vector<mir::RenderView> &views)
    {
        std::vector<float> tags;
        for (const mir::RenderView &view : views)
            tags.push_back(view.fovYDegrees);
        return tags;
    }
}

TEST_CASE("A frame's screen views keep their order and slots, and target views follow them", "[renderer][rendertarget]")
{
    const std::vector<mir::RenderView> views{ Target(10, 0), Screen(1), Target(11, 1), Screen(2) };
    std::vector<mir::RenderView> screen;
    std::vector<mir::RenderView> targets;

    CHECK(mir::SplitFrameViews(views, 16, screen, targets) == 0);
    CHECK(Tags(screen) == std::vector<float>{ 1, 2 });
    CHECK(Tags(targets) == std::vector<float>{ 10, 11 });
}

TEST_CASE("Target views only get the view slots the screen leaves", "[renderer][rendertarget]")
{
    std::vector<mir::RenderView> views;
    for (int i = 0; i < 3; ++i)
        views.push_back(Screen(float(i)));
    views.push_back(Target(10, 0));
    views.push_back(Target(11, 1));
    views.push_back(Target(12, 2));
    std::vector<mir::RenderView> screen;
    std::vector<mir::RenderView> targets;

    SECTION("one slot left")
    {
        CHECK(mir::SplitFrameViews(views, 4, screen, targets) == 2);
        CHECK(screen.size() == 3);
        CHECK(Tags(targets) == std::vector<float>{ 10 });
    }
    SECTION("the screen takes every slot, and more")
    {
        CHECK(mir::SplitFrameViews(views, 2, screen, targets) == 3);
        CHECK(screen.size() == 3);
        CHECK(targets.empty());
    }
}

TEST_CASE("Splitting a frame's views clears the last frame's", "[renderer][rendertarget]")
{
    std::vector<mir::RenderView> screen{ Screen(1) };
    std::vector<mir::RenderView> targets{ Target(10, 0) };

    CHECK(mir::SplitFrameViews({}, 16, screen, targets) == 0);
    CHECK(screen.empty());
    CHECK(targets.empty());
}

namespace
{
    // stands in for the renderer, and remembers what it was asked
    struct FakeHost
    {
        uint32_t next = 0;
        uint32_t live = 0;
        bool refuse = false;
        std::vector<VkExtent2D> created;
        std::vector<std::pair<uint32_t, VkExtent2D>> resized;
        std::vector<uint32_t> released;

        mir::RenderTargetHost Host()
        {
            return {
                .mCreate = [this](VkExtent2D extent)
                    {
                        created.push_back(extent);
                        if (refuse)
                            return mir::kNullRenderTarget;
                        ++live;
                        return mir::RenderTargetHandle{ .mIndex = next++ };
                    },
                .mResize = [this](mir::RenderTargetHandle target, VkExtent2D extent)
                    {
                        resized.emplace_back(target.mIndex, extent);
                        return !refuse;
                    },
                .mRelease = [this](mir::RenderTargetHandle target)
                    {
                        released.push_back(target.mIndex);
                        --live;
                    },
                .mLiveCount = [this] { return live; } };
        }
    };

    mir::Entity MakeTargetCamera(mir::World &world, int32_t width, int32_t height)
    {
        const mir::Entity entity = world.CreateEntity();
        mir::AddTransform(world, entity);
        world.AddComponent<mir::Camera>(entity, mir::Camera{});
        world.AddComponent<mir::RenderTexture>(entity, mir::RenderTexture{ width, height });
        return entity;
    }

    bool Same(VkExtent2D a, VkExtent2D b) { return a.width == b.width && a.height == b.height; }
}

TEST_CASE("A RenderTexture camera gets a target at its clamped size, and only one with a Camera does",
    "[renderer][rendertarget]")
{
    mir::World world;
    const mir::Entity camera = MakeTargetCamera(world, 0, 100000);
    const mir::Entity noCamera = world.CreateEntity();
    mir::AddTransform(world, noCamera);
    world.AddComponent<mir::RenderTexture>(noCamera, mir::RenderTexture{});
    FakeHost fake;

    const std::vector<mir::TargetCamera> cameras = mir::SyncTargetCameras(world, {}, fake.Host());

    REQUIRE(cameras.size() == 1);
    CHECK(cameras[0].mCamera == camera);
    CHECK(Same(cameras[0].mExtent, VkExtent2D{ 1, mir::kMaxRenderTargetSize }));
    REQUIRE(fake.created.size() == 1);
    CHECK(Same(fake.created[0], VkExtent2D{ 1, mir::kMaxRenderTargetSize }));
}

TEST_CASE("A RenderTexture camera keeps its target from frame to frame, resized when its size changes",
    "[renderer][rendertarget]")
{
    mir::World world;
    const mir::Entity camera = MakeTargetCamera(world, 64, 32);
    FakeHost fake;
    std::vector<mir::TargetCamera> cameras = mir::SyncTargetCameras(world, {}, fake.Host());
    cameras = mir::SyncTargetCameras(world, cameras, fake.Host());
    CHECK(fake.created.size() == 1);
    CHECK(fake.resized.empty());

    world.GetComponent<mir::RenderTexture>(camera)->mWidth = 128;
    SECTION("resized")
    {
        cameras = mir::SyncTargetCameras(world, cameras, fake.Host());
        REQUIRE(fake.resized.size() == 1);
        CHECK(fake.resized[0].first == 0);
        CHECK(Same(cameras[0].mExtent, VkExtent2D{ 128, 32 }));
        CHECK(cameras[0].mTarget.mIndex == 0);
    }
    SECTION("a refused resize keeps the old size and asks again")
    {
        fake.refuse = true;
        cameras = mir::SyncTargetCameras(world, cameras, fake.Host());
        CHECK(Same(cameras[0].mExtent, VkExtent2D{ 64, 32 }));
        cameras = mir::SyncTargetCameras(world, cameras, fake.Host());
        CHECK(fake.resized.size() == 2);
    }
}

TEST_CASE("A RenderTexture camera's target is released once the camera, or its RenderTexture, goes",
    "[renderer][rendertarget]")
{
    mir::World world;
    const mir::Entity kept = MakeTargetCamera(world, 16, 16);
    const mir::Entity gone = MakeTargetCamera(world, 16, 16);
    FakeHost fake;
    std::vector<mir::TargetCamera> cameras = mir::SyncTargetCameras(world, {}, fake.Host());
    REQUIRE(cameras.size() == 2);

    SECTION("destroyed")
    {
        world.DestroyEntity(gone);
    }
    SECTION("its RenderTexture removed")
    {
        world.RemoveComponent<mir::RenderTexture>(gone);
    }
    cameras = mir::SyncTargetCameras(world, cameras, fake.Host());
    REQUIRE(cameras.size() == 1);
    CHECK(cameras[0].mCamera == kept);
    CHECK(fake.released == std::vector<uint32_t>{ 1 });
}

TEST_CASE("A RenderTexture camera the renderer refuses a target gets one once a camera gone frees a slot",
    "[renderer][rendertarget]")
{
    mir::World world;
    const mir::Entity first = MakeTargetCamera(world, 16, 16);
    FakeHost fake;
    std::vector<mir::TargetCamera> cameras = mir::SyncTargetCameras(world, {}, fake.Host());

    // the renderer is full
    fake.refuse = true;
    const mir::Entity waiting = MakeTargetCamera(world, 16, 16);
    cameras = mir::SyncTargetCameras(world, cameras, fake.Host());
    REQUIRE(cameras.size() == 2);
    CHECK(cameras[1].mTarget.IsNull());
    cameras = mir::SyncTargetCameras(world, cameras, fake.Host());
    CHECK(fake.created.size() == 2);

    // released before the retry, in the same sync
    fake.refuse = false;
    world.DestroyEntity(first);
    cameras = mir::SyncTargetCameras(world, cameras, fake.Host());
    REQUIRE(cameras.size() == 1);
    CHECK(cameras[0].mCamera == waiting);
    CHECK_FALSE(cameras[0].mTarget.IsNull());
    CHECK(fake.released == std::vector<uint32_t>{ 0 });
    CHECK(fake.created.size() == 3);
}

TEST_CASE("A RenderTexture camera the renderer refuses a target is not asked for again until its size changes",
    "[renderer][rendertarget]")
{
    mir::World world;
    const mir::Entity camera = MakeTargetCamera(world, 16, 16);
    FakeHost fake;
    fake.refuse = true;
    std::vector<mir::TargetCamera> cameras = mir::SyncTargetCameras(world, {}, fake.Host());
    REQUIRE(cameras.size() == 1);
    CHECK(cameras[0].mTarget.IsNull());
    CHECK_FALSE(mir::ResolveTargetCamera(world, cameras[0]).has_value());

    cameras = mir::SyncTargetCameras(world, cameras, fake.Host());
    CHECK(fake.created.size() == 1);

    fake.refuse = false;
    world.GetComponent<mir::RenderTexture>(camera)->mHeight = 32;
    cameras = mir::SyncTargetCameras(world, cameras, fake.Host());
    CHECK(fake.created.size() == 2);
    CHECK_FALSE(cameras[0].mTarget.IsNull());
    CHECK(fake.released.empty());
}

TEST_CASE("A RenderTexture camera's view is at its image's aspect, under a viewport id apart from the screen's",
    "[renderer][rendertarget]")
{
    mir::World world;
    const mir::Entity camera = MakeTargetCamera(world, 200, 100);
    FakeHost fake;
    fake.next = 3;
    const std::vector<mir::TargetCamera> cameras = mir::SyncTargetCameras(world, {}, fake.Host());

    const std::optional<mir::ViewCamera> view = mir::ResolveTargetCamera(world, cameras[0]);
    REQUIRE(view.has_value());
    CHECK(view->viewport == mir::kFirstTargetViewport + 3);
    CHECK(Same(view->extent, VkExtent2D{ 200, 100 }));
    // x scale over y scale is the inverse aspect
    CHECK(std::abs(view->viewProj[0][0] / view->viewProj[1][1]) == Catch::Approx(0.5f));

    world.RemoveComponent<mir::Camera>(camera);
    CHECK_FALSE(mir::ResolveTargetCamera(world, cameras[0]).has_value());
}
