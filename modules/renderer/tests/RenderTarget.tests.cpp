/**
 * @file RenderTarget.tests.cpp
 * @author Quy Dong Ta
 * @brief Tests for how a frame's views share the view slots between the screen and render targets.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer/VulkanRenderer.h>

#include <catch2/catch_test_macros.hpp>

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
