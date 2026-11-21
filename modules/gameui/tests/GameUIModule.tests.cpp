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
#include <gameui/UIWidgets.h>

#include <core/ecs/SystemScheduler.h>
#include <core/ecs/World.h>
#include <input/InputMap.h>
#include <input/InputState.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <vector>

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
    const Registration screen = ui.AddScreen(UIScreenLayer::Game, [&runs](UIContext &, float) { ++runs; });
    ui.RunFrame(UIRect{ 0, 0, 100, 100 }, UIPointer{ 5, 5, true });
    ui.RunFrame(UIRect{ 0, 0, 100, 100 }, UIPointer{ 5, 5, true });
    CHECK(runs == 2);
}

TEST_CASE("A suspended game UI runs no screen, so nothing it draws can be clicked", "[gameui][module]")
{
    Rig rig;
    GameUIModule ui(ModuleContext{ rig.world, rig.scheduler });
    int runs = 0;
    const Registration screen = ui.AddScreen(UIScreenLayer::Game, [&runs](UIContext &, float) { ++runs; });

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
    const Registration screen = ui.AddScreen(UIScreenLayer::Game, [](UIContext &context, float)
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

TEST_CASE("Shell screens draw over Game screens, and a Shell screen holding the input disables the Game layer's widgets", "[gameui][module]")
{
    Rig rig;
    GameUIModule ui(ModuleContext{ rig.world, rig.scheduler });
    std::vector<int> order;
    bool shellHolds = false;
    bool gameHolds = false;
    UIInteraction hud;
    // registered first, drawn last
    const Registration shell = ui.AddScreen(UIScreenLayer::Shell, [&](UIContext &context, float)
        {
            order.push_back(2);
            if (shellHolds)
                context.HoldInput();
        });
    const Registration game = ui.AddScreen(UIScreenLayer::Game, [&](UIContext &context, float)
        {
            order.push_back(1);
            if (gameHolds)
                context.HoldInput();
            hud = context.Interact(UIId("hud"), UIRect{ 100, 100, 200, 50 });
        });
    constexpr UIRect kArea{ 0, 0, 640, 480 };
    const auto click = [&]
    {
        ui.RunFrame(kArea, UIPointer{ 150, 120, false });
        ui.RunFrame(kArea, UIPointer{ 150, 120, true });
        ui.RunFrame(kArea, UIPointer{ 150, 120, false });
        return hud.mClicked;
    };

    ui.RunFrame(kArea, UIPointer{ 150, 120, false });
    CHECK(order == std::vector<int>{ 1, 2 });
    CHECK(click());

    // a game's own screen holding the input is not the shell's
    gameHolds = true;
    ui.RunFrame(kArea, UIPointer{ 150, 120, false });
    CHECK_FALSE(ui.ShellHeldInput());
    CHECK(click());
    gameHolds = false;

    shellHolds = true;
    ui.RunFrame(kArea, UIPointer{ 150, 120, false });
    CHECK(ui.ShellHeldInput());
    CHECK_FALSE(click());

    // a frame later the game's widgets are live again
    shellHolds = false;
    ui.RunFrame(kArea, UIPointer{ 150, 120, false });
    CHECK_FALSE(ui.ShellHeldInput());
    CHECK(click());
}

TEST_CASE("A Game slider held when a Shell screen takes the input lets go and stops following the pointer", "[gameui][module]")
{
    Rig rig;
    GameUIModule ui(ModuleContext{ rig.world, rig.scheduler });
    bool shellHolds = false;
    float value = 0.25f;
    UIWidgetResult slid;
    const Registration shell = ui.AddScreen(UIScreenLayer::Shell, [&](UIContext &context, float)
        {
            if (shellHolds)
                context.HoldInput();
        });
    const Registration game = ui.AddScreen(UIScreenLayer::Game, [&](UIContext &context, float)
        { slid = Slider(context, UIId("level"), UIRect{ 100, 100, 200, 30 }, value, 0.0f, 1.0f); });
    constexpr UIRect kArea{ 0, 0, 640, 480 };

    ui.RunFrame(kArea, UIPointer{ 150, 115, false });
    ui.RunFrame(kArea, UIPointer{ 150, 115, true });
    ui.RunFrame(kArea, UIPointer{ 160, 115, true });
    REQUIRE(slid.mHeld);
    CHECK(value == Catch::Approx(0.3f));

    // the menu opens mid-drag: this frame's Game screens ran before it
    shellHolds = true;
    ui.RunFrame(kArea, UIPointer{ 170, 115, true });
    CHECK(value == Catch::Approx(0.35f));

    ui.RunFrame(kArea, UIPointer{ 270, 115, true });
    CHECK_FALSE(slid.mHeld);
    CHECK(value == Catch::Approx(0.35f));

    // the menu closes with the button still down: the drag does not come back
    shellHolds = false;
    ui.RunFrame(kArea, UIPointer{ 270, 115, true });
    ui.RunFrame(kArea, UIPointer{ 280, 115, true });
    CHECK_FALSE(slid.mHeld);
    CHECK(value == Catch::Approx(0.35f));
}

TEST_CASE("A Game screen cannot move focus while a Shell screen holds the input", "[gameui][module]")
{
    Rig rig;
    GameUIModule ui(ModuleContext{ rig.world, rig.scheduler });
    bool shellHolds = false;
    bool opened = false;
    bool gameTookFocus = false;
    constexpr UIId kResume("resume");
    constexpr UIId kHud("hud");
    const Registration shell = ui.AddScreen(UIScreenLayer::Shell, [&](UIContext &context, float)
        {
            if (!shellHolds)
                return;
            context.HoldInput();
            if (!opened)
                context.SetFocus(kResume);
            opened = true;
            context.Interact(kResume, UIRect{ 300, 300, 100, 40 });
        });
    const Registration game = ui.AddScreen(UIScreenLayer::Game, [&](UIContext &context, float)
        {
            gameTookFocus = context.SetFocus(kHud);
            context.Interact(kHud, UIRect{ 10, 10, 100, 40 });
        });
    constexpr UIRect kArea{ 0, 0, 640, 480 };

    ui.RunFrame(kArea, UIPointer{});
    CHECK(gameTookFocus);
    CHECK(ui.Context().State().Focus() == kHud);

    shellHolds = true;
    ui.RunFrame(kArea, UIPointer{});
    CHECK(ui.Context().State().Focus() == kResume);
    for (int frame = 0; frame < 3; ++frame)
    {
        ui.RunFrame(kArea, UIPointer{});
        CHECK_FALSE(gameTookFocus);
        CHECK(ui.Context().State().Focus() == kResume);
    }
}
