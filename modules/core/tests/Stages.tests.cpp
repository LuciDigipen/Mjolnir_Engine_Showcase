/**
 * @file Stages.tests.cpp
 * @author sumin.park
 * @brief Tests for SystemScheduler stages, frame slots and their registration tokens.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <core/ecs/CommandBuffer.h>
#include <core/Registration.h>
#include <core/ecs/SystemScheduler.h>
#include <core/ecs/World.h>

#include <catch2/catch_test_macros.hpp>

#include <stdexcept>
#include <utility>
#include <vector>

using namespace mir;

namespace
{
    struct StageSpawned
    {
        int tag = 0;
    };
}

TEST_CASE("A stage runs its functions in registration order", "[ecs][stage]")
{
    World world;
    CommandBuffer commands;
    SystemScheduler scheduler;
    std::vector<int> calls;

    const Registration first = scheduler.AddStage(Stage::SceneLoad, [&](SystemContext &) { calls.push_back(1); });
    const Registration second = scheduler.AddStage(Stage::SceneLoad, [&](SystemContext &) { calls.push_back(2); });
    const Registration other = scheduler.AddStage(Stage::PlayBegin, [&](SystemContext &) { calls.push_back(9); });

    SystemContext context{ world, commands };
    scheduler.RunStage(Stage::SceneLoad, context);

    REQUIRE(calls == std::vector<int>{ 1, 2 });
    REQUIRE(scheduler.FunctionCount(Stage::SceneLoad) == 2);
    REQUIRE(scheduler.FunctionCount(Stage::SceneUnload) == 0);
}

TEST_CASE("Lifecycle callbacks create entities under their scene instance", "[ecs][stage]")
{
    World world;
    CommandBuffer commands;
    SystemScheduler scheduler;
    const auto current = AllocateSceneInstanceId();
    const auto outgoing = AllocateSceneInstanceId();
    world.SetActiveScene(current);
    Entity spawned;
    const Registration callback = scheduler.AddStage(Stage::SceneEnd, [&](SystemContext &context)
        {
            CHECK(context.sceneInstance == outgoing);
            spawned = context.world.CreateEntity();
            context.commands.Add(spawned, StageSpawned{ 3 });
        });
    SystemContext context{ world, commands };
    context.sceneInstance = outgoing;
    scheduler.RunStage(Stage::SceneEnd, context);
    CHECK(world.EntityScene(spawned) == outgoing);
    CHECK(world.ActiveScene() == current);
    REQUIRE(world.GetComponent<StageSpawned>(spawned) != nullptr);
}

TEST_CASE("A stage flushes the commands its functions recorded", "[ecs][stage]")
{
    World world;
    CommandBuffer commands;
    SystemScheduler scheduler;

    const Registration spawn = scheduler.AddStage(Stage::SceneLoad, [](SystemContext &context)
        {
            const Entity entity = context.world.CreateEntity();
            context.commands.Add(entity, StageSpawned{ 1 });
        });

    SystemContext context{ world, commands };
    scheduler.RunStage(Stage::SceneLoad, context);

    std::size_t count = 0;
    world.ForEach<StageSpawned>([&](Entity, StageSpawned &) { ++count; });
    REQUIRE(count == 1);
}

TEST_CASE("A registration token unregisters when it dies or resets", "[ecs][stage]")
{
    World world;
    CommandBuffer commands;
    SystemScheduler scheduler;
    SystemContext context{ world, commands };
    int calls = 0;

    {
        const Registration token = scheduler.AddStage(Stage::PlayEnd, [&](SystemContext &) { ++calls; });
        scheduler.RunStage(Stage::PlayEnd, context);
    }
    scheduler.RunStage(Stage::PlayEnd, context);
    REQUIRE(calls == 1);
    REQUIRE(scheduler.FunctionCount(Stage::PlayEnd) == 0);

    Registration token = scheduler.AddStage(Stage::PlayEnd, [&](SystemContext &) { ++calls; });
    REQUIRE(token.Active());
    token.Reset();
    REQUIRE_FALSE(token.Active());
    scheduler.RunStage(Stage::PlayEnd, context);
    REQUIRE(calls == 1);
}

TEST_CASE("A moved token keeps the registration and the moved-from one does nothing", "[ecs][stage]")
{
    World world;
    CommandBuffer commands;
    SystemScheduler scheduler;
    SystemContext context{ world, commands };
    int calls = 0;

    Registration moved;
    {
        Registration original = scheduler.AddStage(Stage::SceneSaved, [&](SystemContext &) { ++calls; });
        moved = std::move(original);
        REQUIRE_FALSE(original.Active());
    }

    scheduler.RunStage(Stage::SceneSaved, context);
    REQUIRE(calls == 1);

    moved = Registration{};
    scheduler.RunStage(Stage::SceneSaved, context);
    REQUIRE(calls == 1);
}

TEST_CASE("A throwing stage function stops the stage and reaches the caller", "[ecs][stage]")
{
    World world;
    CommandBuffer commands;
    SystemScheduler scheduler;
    SystemContext context{ world, commands };
    std::vector<int> calls;

    const Registration veto = scheduler.AddStage(Stage::ReloadPrepare, [&](SystemContext &)
        {
            calls.push_back(1);
            throw std::runtime_error("rejected");
        });
    const Registration later = scheduler.AddStage(Stage::ReloadPrepare, [&](SystemContext &) { calls.push_back(2); });

    REQUIRE_THROWS_AS(scheduler.RunStage(Stage::ReloadPrepare, context), std::runtime_error);
    REQUIRE(calls == std::vector<int>{ 1 });

    // the stage is no longer running, so registering is allowed again
    const Registration after = scheduler.AddStage(Stage::ReloadCommit, [&](SystemContext &) { calls.push_back(3); });
    scheduler.RunStage(Stage::ReloadCommit, context);
    REQUIRE(calls == std::vector<int>{ 1, 3 });
}

TEST_CASE("A frame slot runs its functions with the frame's context", "[ecs][stage]")
{
    World world;
    CommandBuffer commands;
    SystemScheduler scheduler;
    std::vector<float> deltas;

    const Registration slot =
        scheduler.AddFrameSlot(FrameSlot::FrameBegin, [&](SystemContext &context) { deltas.push_back(context.dt); });
    REQUIRE(scheduler.FunctionCount(FrameSlot::FrameBegin) == 1);

    SystemContext context{ world, commands, 0.25f };
    scheduler.RunFrameSlot(FrameSlot::FrameBegin, context);

    REQUIRE(deltas == std::vector<float>{ 0.25f });
}
