/**
 * @file EditorExtension.tests.cpp
 * @author Jackey Cen
 * @brief Tests for editor/EditorExtension.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <editor/EditorExtension.h>

#include <core/ecs/EngineMode.h>
#include <core/ecs/World.h>
#include <editor/EditorCamera.h>
#include <editor/history/History.h>

#include <catch2/catch_test_macros.hpp>

#include <span>
#include <string>
#include <utility>
#include <vector>

namespace
{
    using mir::EditorCamera;
    using mir::EditorExtensionContext;
    using mir::EditorExtensions;
    using mir::EngineModeState;
    using mir::History;
    using mir::IEditorExtension;
    using mir::kSceneDocument;
    using mir::PixelRect;
    using mir::RawInputSnapshot;
    using mir::World;

    struct Fixture
    {
        World mWorld;
        History mHistory;
        EngineModeState mMode;
        EditorCamera mCamera;
        RawInputSnapshot mInput;
        PixelRect mViewport;

        EditorExtensionContext Context()
        {
            return EditorExtensionContext{ mMode, kSceneDocument, mWorld, mViewport, mCamera, mInput, false, nullptr,
                mHistory };
        }
    };

    struct FakeExtension final : IEditorExtension
    {
        FakeExtension(std::string name, std::vector<std::string> &events)
            : mName(std::move(name)), mEvents(events)
        {
        }

        void Update(const EditorExtensionContext &) override { mEvents.push_back(mName + ".update"); }

        std::span<const char *const> WindowNames() const override
        {
            return std::span<const char *const>(mWindows);
        }

        void DrawWindows(const EditorExtensionContext &) override { mEvents.push_back(mName + ".windows"); }
        void DrawToolbar(const EditorExtensionContext &) override { mEvents.push_back(mName + ".toolbar"); }
        bool OwnsViewport() const override { return mOwnsViewport; }
        void DrawOverlay(const EditorExtensionContext &) override { mEvents.push_back(mName + ".overlay"); }

        std::string mName;
        std::vector<std::string> &mEvents;
        bool mOwnsViewport = false;
        std::vector<const char *> mWindows;
    };
}

TEST_CASE("Extensions run in registration order", "[editor][extension]")
{
    Fixture fixture;
    EditorExtensions extensions;
    std::vector<std::string> events;
    FakeExtension first("first", events);
    FakeExtension second("second", events);
    const mir::Registration firstToken = extensions.Add(first);
    const mir::Registration secondToken = extensions.Add(second);

    const EditorExtensionContext context = fixture.Context();
    extensions.Update(context);
    extensions.DrawWindows(context);
    extensions.DrawToolbar(context);
    extensions.DrawOverlay(context);

    const std::vector<std::string> expected{
        "first.update", "second.update",
        "first.windows", "second.windows",
        "first.toolbar", "second.toolbar",
        "first.overlay", "second.overlay",
    };
    CHECK(events == expected);
}

TEST_CASE("A registration removes its extension when it dies", "[editor][extension]")
{
    Fixture fixture;
    EditorExtensions extensions;
    std::vector<std::string> events;
    FakeExtension first("first", events);
    FakeExtension second("second", events);
    const mir::Registration secondToken = extensions.Add(second);
    {
        const mir::Registration firstToken = extensions.Add(first);
        const EditorExtensionContext context = fixture.Context();
        extensions.Update(context);
        CHECK(events == std::vector<std::string>{ "second.update", "first.update" });
    }

    events.clear();
    const EditorExtensionContext context = fixture.Context();
    extensions.Update(context);

    CHECK(events == std::vector<std::string>{ "second.update" });
}

TEST_CASE("A moved registration keeps its extension registered", "[editor][extension]")
{
    Fixture fixture;
    EditorExtensions extensions;
    std::vector<std::string> events;
    FakeExtension first("first", events);
    FakeExtension second("second", events);
    mir::Registration token = extensions.Add(first);
    mir::Registration moved = std::move(token);
    token = extensions.Add(second);

    const EditorExtensionContext context = fixture.Context();
    extensions.Update(context);

    CHECK(events == std::vector<std::string>{ "first.update", "second.update" });
}

TEST_CASE("The viewport-owning flag hides the gizmo, shortcuts and picking", "[editor][extension]")
{
    Fixture fixture;
    EditorExtensions extensions;
    std::vector<std::string> events;
    FakeExtension ordinary("ordinary", events);
    FakeExtension owner("owner", events);
    owner.mOwnsViewport = true;

    const mir::Registration ordinaryToken = extensions.Add(ordinary);
    CHECK_FALSE(extensions.OwnsViewport());

    mir::Registration ownerToken = extensions.Add(owner);
    CHECK(extensions.OwnsViewport());

    owner.mOwnsViewport = false;
    CHECK_FALSE(extensions.OwnsViewport());

    owner.mOwnsViewport = true;
    ownerToken.Reset();
    CHECK_FALSE(extensions.OwnsViewport());
}

TEST_CASE("Window names come from every extension in registration order", "[editor][extension]")
{
    EditorExtensions extensions;
    std::vector<std::string> events;
    FakeExtension first("first", events);
    FakeExtension second("second", events);
    first.mWindows = { "First" };
    second.mWindows = { "Second", "SecondTool" };
    const mir::Registration firstToken = extensions.Add(first);
    const mir::Registration secondToken = extensions.Add(second);

    const std::vector<const char *> names = extensions.WindowNames();

    REQUIRE(names.size() == 3);
    CHECK(std::string(names[0]) == "First");
    CHECK(std::string(names[1]) == "Second");
    CHECK(std::string(names[2]) == "SecondTool");
}
