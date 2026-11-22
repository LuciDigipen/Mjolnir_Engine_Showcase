/**
 * @file ActionsModule.tests.cpp
 * @author Quy Dong Ta
 * @brief Tests which clock runs when, through the module's system, frame slot and stage.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <actions/ActionScheduler.h>
#include <actions/ActionsModule.h>

#include <core/ecs/CommandBuffer.h>
#include <core/ecs/EngineMode.h>
#include <core/ecs/SystemScheduler.h>
#include <core/ecs/World.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace mir;

namespace
{
    struct ModuleFixture
    {
        World world;
        CommandBuffer commands;
        SystemScheduler scheduler;
        SystemContext context{ world, commands, 0.25f, 0.0, 0 };
        EngineModeState &mode = world.EmplaceResource<EngineModeState>();
        ActionsModule module{ ModuleContext{ world, scheduler } };

        ModuleFixture() { scheduler.Start(context); }

        ~ModuleFixture()
        {
            if (scheduler.Started())
                scheduler.Stop(context);
        }

        ActionScheduler &Actions() { return world.Resource<ActionScheduler>(); }

        void Frame()
        {
            scheduler.RunFrameSlot(FrameSlot::FrameBegin, context);
            scheduler.Update(context);
        }
    };
}

TEST_CASE("The module puts the scheduler in the World", "[actions][module]")
{
    ModuleFixture f;
    CHECK(&f.module.Scheduler() == f.world.TryResource<ActionScheduler>());
}

TEST_CASE("The Game clock runs only while the simulation runs, the UI clock every frame", "[actions][module][clock]")
{
    ModuleFixture f;
    f.Actions().TweenValue("Game", 1.0f, { .mDuration = 1.0f });
    f.Actions().TweenValue("UI", 1.0f, { .mDuration = 1.0f, .mClock = ActionClock::UI });

    SECTION("Editing")
    {
        f.Frame();
        CHECK(f.Actions().Value("Game", -1.0f) == -1.0f);
        CHECK(f.Actions().Value("UI") == Catch::Approx(0.25f));
    }

    SECTION("Paused")
    {
        SetEngineMode(f.world, EngineMode::Play);
        SetPlayState(f.world, PlayState::Paused);
        f.Frame();
        CHECK(f.Actions().Value("Game", -1.0f) == -1.0f);
        CHECK(f.Actions().Value("UI") == Catch::Approx(0.25f));
    }

    SECTION("Playing")
    {
        SetEngineMode(f.world, EngineMode::Play);
        f.Frame();
        CHECK(f.Actions().Value("Game") == Catch::Approx(0.25f));
        CHECK(f.Actions().Value("UI") == Catch::Approx(0.25f));
    }
}

TEST_CASE("PlayEnd clears every action and value", "[actions][module]")
{
    ModuleFixture f;
    f.Actions().TweenValue("Fade", 1.0f);
    f.Actions().SetValue("Kept", 1.0f);

    f.scheduler.RunStage(Stage::PlayEnd, f.context);
    CHECK(f.Actions().Count() == 0);
    CHECK_FALSE(f.Actions().HasValue("Kept"));
}
