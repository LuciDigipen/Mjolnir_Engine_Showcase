/**
 * @file GameUIModule.tests.cpp
 * @author sumin.park
 * @brief A suspended game UI runs no screen.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
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
