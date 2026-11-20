/**
 * @file GameUIModule.tests.cpp
 * @author sumin.park
 * @brief A suspended game UI runs no screen, and the game UI says when it has the pointer.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <gameui/GameUIArea.h>
#include <gameui/GameUIModule.h>

#include <core/ecs/SystemScheduler.h>
#include <core/ecs/World.h>
#include <input/InputMap.h>
#include <input/InputState.h>

#include <catch2/catch_test_macros.hpp>

using namespace mir;

namespace
{
    struct Rig
    {
        Rig()
        {
            world.EmplaceResource<InputState>();
            world.EmplaceResource<InputMap>();
        }

        World world;
        SystemScheduler scheduler;
    };
}

TEST_CASE("A game UI runs its screens each frame", "[gameui][module]")
{
    Rig rig;
    GameUIModule ui(ModuleContext{ rig.world, rig.scheduler });
    int runs = 0;
    const Registration screen = ui.AddScreen([&runs](UIContext &, float) { ++runs; });
    ui.RunFrame(UIRect{ 0, 0, 100, 100 }, UIPointer{ 5, 5, true });
    ui.RunFrame(UIRect{ 0, 0, 100, 100 }, UIPointer{ 5, 5, true });
    CHECK(runs == 2);
}

TEST_CASE("A suspended game UI runs no screen, so nothing it draws can be clicked", "[gameui][module]")
{
    Rig rig;
    GameUIModule ui(ModuleContext{ rig.world, rig.scheduler });
    int runs = 0;
    const Registration screen = ui.AddScreen([&runs](UIContext &, float) { ++runs; });

    ui.SetSuspended(true);
    CHECK(ui.Suspended());
    ui.RunFrame(UIRect{ 0, 0, 100, 100 }, UIPointer{ 5, 5, true });
    CHECK(runs == 0);

    // and it picks up where it was when the scene is ready
    ui.SetSuspended(false);
    ui.RunFrame(UIRect{ 0, 0, 100, 100 }, UIPointer{ 5, 5, false });
    CHECK(runs == 1);
}

TEST_CASE("The game UI says when a widget is under the pointer or held by it", "[gameui][module]")
{
    Rig rig;
    GameUIModule ui(ModuleContext{ rig.world, rig.scheduler });
    const Registration screen = ui.AddScreen([](UIContext &context, float)
        { context.Interact(UIId("button"), UIRect{ 100, 100, 200, 50 }); });
    const GameUIPointer &pointer = rig.world.Resource<GameUIPointer>();
    constexpr UIRect kArea{ 0, 0, 640, 480 };

    ui.RunFrame(kArea, UIPointer{ 10, 10, false });
    CHECK_FALSE(pointer.mOverWidget);

    ui.RunFrame(kArea, UIPointer{ 150, 120, false });
    CHECK(pointer.mOverWidget);

    // pressed, then dragged off: still the game UI's until release
    ui.RunFrame(kArea, UIPointer{ 150, 120, true });
    ui.RunFrame(kArea, UIPointer{ 10, 10, true });
    CHECK(pointer.mOverWidget);
    ui.RunFrame(kArea, UIPointer{ 10, 10, false });
    CHECK_FALSE(pointer.mOverWidget);

    ui.RunFrame(kArea, UIPointer{ 150, 120, false, true });
    CHECK_FALSE(pointer.mOverWidget);

    ui.RunFrame(kArea, UIPointer{ 150, 120, false });
    REQUIRE(pointer.mOverWidget);
    ui.SetSuspended(true);
    ui.RunFrame(kArea, UIPointer{ 150, 120, false });
    CHECK_FALSE(pointer.mOverWidget);
}
