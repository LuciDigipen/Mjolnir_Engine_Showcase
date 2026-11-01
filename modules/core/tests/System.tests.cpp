/**
 * @file System.tests.cpp
 * @author Sumin Park
 * @brief Tests for core/ecs/SystemScheduler ordering and lifecycle.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <core/ecs/CommandBuffer.h>
#include <core/ecs/System.h>
#include <core/ecs/SystemScheduler.h>
#include <core/ecs/World.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <vector>

using namespace mir;

namespace
{
    struct SPosition
    {
        float x;
        float y;
    };

    struct SSpawned
    {
        int tag;
    };

    struct Recorder
    {
        std::vector<int> started;
        std::vector<int> updated;
        std::vector<int> stopped;
    };

    // Writes its id into the recorder at every lifecycle point, so ordering is
    // observable without any world state.
    class TagSystem final : public ISystem
    {
    public:
        TagSystem(Recorder &recorder, int id) : mRecorder(recorder), mId(id) {}

        void OnStart(SystemContext &) override { mRecorder.started.push_back(mId); }
        void OnUpdate(SystemContext &) override { mRecorder.updated.push_back(mId); }
        void OnStop(SystemContext &) override { mRecorder.stopped.push_back(mId); }

    private:
        Recorder &mRecorder;
        int mId;
    };

    // Spawns one entity per tick through the command buffer.
    class SpawnSystem final : public ISystem
    {
    public:
        void OnUpdate(SystemContext &context) override
        {
            const Entity entity = context.world.CreateEntity();
            context.commands.Add(entity, SSpawned{mNextTag++});
        }

    private:
        int mNextTag = 0;
    };

    // Counts what is visible to it at the moment it runs.
    class CountSystem final : public ISystem
    {
    public:
        void OnUpdate(SystemContext &context) override
        {
            std::size_t count = 0;
            context.world.ForEach<SSpawned>([&](Entity, SSpawned &)
                                            { ++count; });
            seen.push_back(count);
        }

        std::vector<std::size_t> seen;
    };
}

TEST_CASE("Systems run in phase order, then by order inside a phase", "[ecs][system]")
{
    World world;
    CommandBuffer commands;
    SystemScheduler scheduler;
    Recorder recorder;

    // registered out of phase and order on purpose
    scheduler.AddSystem<TagSystem>(SystemPhase::Update, SystemOrder{SystemOrder::kLate}, recorder, 1);
    scheduler.AddSystem<TagSystem>(SystemPhase::PostUpdate, SystemOrder{SystemOrder::kNormal}, recorder, 4);
    scheduler.AddSystem<TagSystem>(SystemPhase::Update, SystemOrder{SystemOrder::kEarly}, recorder, 2);
    scheduler.AddSystem<TagSystem>(SystemPhase::PreUpdate, SystemOrder{SystemOrder::kNormal}, recorder, 3);
    scheduler.AddSystem<TagSystem>(SystemPhase::Update, SystemOrder{SystemOrder::kNormal}, recorder, 5);
    scheduler.AddSystem<TagSystem>(SystemPhase::Update, SystemOrder{SystemOrder::kEarly + 5}, recorder, 6);

    REQUIRE(scheduler.SystemCount() == 6);
    REQUIRE(scheduler.SystemCount(SystemPhase::Update) == 4);
    REQUIRE(scheduler.SystemCount(SystemPhase::Render) == 0);

    SystemContext context{world, commands};
    scheduler.Start(context);
    scheduler.Update(context);

    // Update by order: 2 (10), 6 (15), 5 (50), 1 (90)
    const std::vector<int> expected{3, 2, 6, 5, 1, 4};
    REQUIRE(recorder.started == expected);
    REQUIRE(recorder.updated == expected);
}

TEST_CASE("OnStop follows the sorted order, and the bounds are inclusive", "[ecs][system]")
{
    World world;
    CommandBuffer commands;
    SystemScheduler scheduler;
    Recorder recorder;

    scheduler.AddSystem<TagSystem>(SystemPhase::Update, SystemOrder{SystemOrder::kMax}, recorder, 1);
    scheduler.AddSystem<TagSystem>(SystemPhase::Update, SystemOrder{SystemOrder::kMin}, recorder, 2);

    SystemContext context{world, commands};
    scheduler.Start(context);
    scheduler.Stop(context);

    REQUIRE(recorder.started == std::vector<int>{2, 1});
    REQUIRE(recorder.stopped == std::vector<int>{2, 1});
}

TEST_CASE("FixedUpdate keeps the sorted order in every substep", "[ecs][system]")
{
    World world;
    CommandBuffer commands;
    SystemScheduler scheduler;
    Recorder recorder;

    scheduler.AddSystem<TagSystem>(SystemPhase::FixedUpdate, SystemOrder{SystemOrder::kNormal}, recorder, 1);
    scheduler.AddSystem<TagSystem>(SystemPhase::FixedUpdate, SystemOrder{SystemOrder::kEarly}, recorder, 2);

    SystemContext context{world, commands};
    scheduler.Start(context);
    scheduler.Update(context, 3, 1.0f / 60.0f);

    REQUIRE(recorder.updated == std::vector<int>{2, 1, 2, 1, 2, 1});
}

TEST_CASE("OnStart runs once and OnStop runs in the same order", "[ecs][system]")
{
    World world;
    CommandBuffer commands;
    SystemScheduler scheduler;
    Recorder recorder;

    scheduler.AddSystem<TagSystem>(SystemPhase::PreUpdate, SystemOrder{SystemOrder::kNormal}, recorder, 1);
    scheduler.AddSystem<TagSystem>(SystemPhase::Update, SystemOrder{SystemOrder::kNormal}, recorder, 2);
    scheduler.AddSystem<TagSystem>(SystemPhase::Update, SystemOrder{SystemOrder::kLate}, recorder, 3);

    SystemContext context{world, commands};
    REQUIRE_FALSE(scheduler.Started());

    scheduler.Start(context);
    REQUIRE(scheduler.Started());

    scheduler.Update(context);
    scheduler.Update(context);

    REQUIRE(recorder.started == std::vector<int>{1, 2, 3});
    REQUIRE(recorder.updated == std::vector<int>{1, 2, 3, 1, 2, 3});

    scheduler.Stop(context);
    REQUIRE(recorder.stopped == std::vector<int>{1, 2, 3});
    REQUIRE_FALSE(scheduler.Started());

    // Stop is idempotent, so App::Shutdown running twice is harmless
    scheduler.Stop(context);
    REQUIRE(recorder.stopped == std::vector<int>{1, 2, 3});
}

TEST_CASE("Commands recorded in one phase are visible to the next", "[ecs][system]")
{
    World world;
    CommandBuffer commands;
    SystemScheduler scheduler;

    scheduler.AddSystem<SpawnSystem>(SystemPhase::PreUpdate, SystemOrder{SystemOrder::kNormal});
    CountSystem &counter = scheduler.AddSystem<CountSystem>(SystemPhase::Update, SystemOrder{SystemOrder::kNormal});

    SystemContext context{world, commands};
    scheduler.Start(context);

    scheduler.Update(context);
    scheduler.Update(context);
    scheduler.Update(context);

    // the entity spawned in PreUpdate is already there when Update runs
    REQUIRE(counter.seen == std::vector<std::size_t>{1, 2, 3});
}

TEST_CASE("A spawn is not visible to systems in its own phase", "[ecs][system]")
{
    World world;
    CommandBuffer commands;
    SystemScheduler scheduler;

    scheduler.AddSystem<SpawnSystem>(SystemPhase::Update, SystemOrder{SystemOrder::kNormal});
    CountSystem &counter = scheduler.AddSystem<CountSystem>(SystemPhase::Update, SystemOrder{SystemOrder::kNormal});

    SystemContext context{world, commands};
    scheduler.Start(context);

    scheduler.Update(context);
    scheduler.Update(context);

    // one phase of latency: the counter always trails the spawner by a frame
    REQUIRE(counter.seen == std::vector<std::size_t>{0, 1});
}

TEST_CASE("A system drops its hook token in OnStop, so Reset leaves no hook behind", "[ecs][system]")
{
    World world;
    CommandBuffer commands;
    int hookCalls = 0;

    struct Hooked
    {
        int value = 0;
    };

    class HookSystem final : public ISystem
    {
    public:
        explicit HookSystem(int &calls) : mCalls(calls) {}
        void OnStart(SystemContext &context) override
        {
            mHooks = context.world.SetComponentHooks<Hooked>({ .onRemove = [this](World &, Entity) { ++mCalls; } });
        }
        void OnUpdate(SystemContext &) override {}
        void OnStop(SystemContext &) override { mHooks.Reset(); }

    private:
        int &mCalls;
        Registration mHooks;
    };

    {
        SystemScheduler scheduler;
        scheduler.AddSystem<HookSystem>(SystemPhase::PreUpdate, SystemOrder{SystemOrder::kNormal}, hookCalls);

        SystemContext context{world, commands};
        scheduler.Start(context);

        const Entity a = world.CreateEntity();
        world.AddComponent(a, Hooked{});
        world.DestroyEntity(a);
        REQUIRE(hookCalls == 1);

        scheduler.Stop(context);
        scheduler.Reset(); // the HookSystem instance is destroyed here
    }

    // If OnStop had not removed the hook, this would call through a
    // dangling `this` - instead the count simply stops moving.
    const Entity b = world.CreateEntity();
    world.AddComponent(b, Hooked{});
    world.DestroyEntity(b);
    REQUIRE(hookCalls == 1);
}

TEST_CASE("The context carries frame timing through to systems", "[ecs][system]")
{
    World world;
    CommandBuffer commands;
    SystemScheduler scheduler;

    class TimeSystem final : public ISystem
    {
    public:
        void OnUpdate(SystemContext &context) override
        {
            deltas.push_back(context.dt);
            frames.push_back(context.frame);
        }

        std::vector<float> deltas;
        std::vector<uint64_t> frames;
    };

    TimeSystem &timing = scheduler.AddSystem<TimeSystem>(SystemPhase::Update, SystemOrder{SystemOrder::kNormal});

    SystemContext context{world, commands};
    scheduler.Start(context);

    context.dt = 0.5f;
    context.frame = 7;
    scheduler.Update(context);

    REQUIRE(timing.deltas == std::vector<float>{0.5f});
    REQUIRE(timing.frames == std::vector<uint64_t>{7});
}

TEST_CASE("FixedUpdate runs after Update and before PostUpdate", "[ecs][system]")
{
    World world;
    CommandBuffer commands;
    SystemScheduler scheduler;
    Recorder recorder;

    // registered out of phase order on purpose
    scheduler.AddSystem<TagSystem>(SystemPhase::Render, SystemOrder{SystemOrder::kNormal}, recorder, 5);
    scheduler.AddSystem<TagSystem>(SystemPhase::FixedUpdate, SystemOrder{SystemOrder::kNormal}, recorder, 3);
    scheduler.AddSystem<TagSystem>(SystemPhase::PreUpdate, SystemOrder{SystemOrder::kNormal}, recorder, 1);
    scheduler.AddSystem<TagSystem>(SystemPhase::PostUpdate, SystemOrder{SystemOrder::kNormal}, recorder, 4);
    scheduler.AddSystem<TagSystem>(SystemPhase::Update, SystemOrder{SystemOrder::kNormal}, recorder, 2);

    SystemContext context{world, commands};
    scheduler.Start(context);
    scheduler.Update(context, 1, 1.0f / 60.0f);
    scheduler.Stop(context);

    REQUIRE(recorder.started == std::vector<int>{1, 2, 3, 4, 5});
    REQUIRE(recorder.updated == std::vector<int>{1, 2, 3, 4, 5});
    REQUIRE(recorder.stopped == std::vector<int>{1, 2, 3, 4, 5});
}

TEST_CASE("FixedUpdate runs once per substep, every other phase once per frame", "[ecs][system]")
{
    World world;
    CommandBuffer commands;
    SystemScheduler scheduler;
    Recorder recorder;

    scheduler.AddSystem<TagSystem>(SystemPhase::PreUpdate, SystemOrder{SystemOrder::kNormal}, recorder, 1);
    scheduler.AddSystem<TagSystem>(SystemPhase::Update, SystemOrder{SystemOrder::kNormal}, recorder, 2);
    scheduler.AddSystem<TagSystem>(SystemPhase::FixedUpdate, SystemOrder{SystemOrder::kNormal}, recorder, 3);
    scheduler.AddSystem<TagSystem>(SystemPhase::PostUpdate, SystemOrder{SystemOrder::kNormal}, recorder, 4);
    scheduler.AddSystem<TagSystem>(SystemPhase::Render, SystemOrder{SystemOrder::kNormal}, recorder, 5);

    SystemContext context{world, commands};
    scheduler.Start(context);

    scheduler.Update(context, 3, 1.0f / 60.0f);
    scheduler.Update(context, 0, 1.0f / 60.0f);

    // frame 1: three substeps; frame 2: shorter than a step, yet Render still runs
    REQUIRE(recorder.updated == std::vector<int>{1, 2, 3, 3, 3, 4, 5, 1, 2, 4, 5});
}

TEST_CASE("FixedUpdate flushes commands after every substep", "[ecs][system]")
{
    World world;
    CommandBuffer commands;
    SystemScheduler scheduler;

    scheduler.AddSystem<SpawnSystem>(SystemPhase::FixedUpdate, SystemOrder{SystemOrder::kNormal});
    CountSystem &fixedCounter = scheduler.AddSystem<CountSystem>(SystemPhase::FixedUpdate, SystemOrder{SystemOrder::kNormal});
    CountSystem &postCounter = scheduler.AddSystem<CountSystem>(SystemPhase::PostUpdate, SystemOrder{SystemOrder::kNormal});

    SystemContext context{world, commands};
    scheduler.Start(context);

    scheduler.Update(context, 2, 1.0f / 60.0f);

    // the first substep's spawn is already there in the second
    REQUIRE(fixedCounter.seen == std::vector<std::size_t>{0, 1});
    // and both are there by PostUpdate
    REQUIRE(postCounter.seen == std::vector<std::size_t>{2});
}

TEST_CASE("FixedUpdate gets the fixed step as dt, other phases keep the frame dt", "[ecs][system]")
{
    World world;
    CommandBuffer commands;
    SystemScheduler scheduler;

    class DeltaSystem final : public ISystem
    {
    public:
        void OnUpdate(SystemContext &context) override { deltas.push_back(context.dt); }

        std::vector<float> deltas;
    };

    DeltaSystem &updateSystem = scheduler.AddSystem<DeltaSystem>(SystemPhase::Update, SystemOrder{SystemOrder::kNormal});
    DeltaSystem &fixedSystem = scheduler.AddSystem<DeltaSystem>(SystemPhase::FixedUpdate, SystemOrder{SystemOrder::kNormal});
    DeltaSystem &postSystem = scheduler.AddSystem<DeltaSystem>(SystemPhase::PostUpdate, SystemOrder{SystemOrder::kNormal});

    SystemContext context{world, commands};
    scheduler.Start(context);

    context.dt = 0.05f;
    scheduler.Update(context, 2, 0.02f);

    REQUIRE(updateSystem.deltas == std::vector<float>{0.05f});
    REQUIRE(fixedSystem.deltas == std::vector<float>{0.02f, 0.02f});
    REQUIRE(postSystem.deltas == std::vector<float>{0.05f});

    // the caller's context keeps the frame dt
    REQUIRE(context.dt == 0.05f);
}

TEST_CASE("Registration ties keep registration order, reversed ties run backwards", "[ecs][system]")
{
    World world;
    CommandBuffer commands;
    SystemContext context{world, commands};

    const auto run = [&](SystemTies ties)
    {
        Recorder recorder;
        SystemScheduler scheduler{ties};
        scheduler.AddSystem<TagSystem>(SystemPhase::Update, SystemOrder{SystemOrder::kNormal}, recorder, 1);
        scheduler.AddSystem<TagSystem>(SystemPhase::Update, SystemOrder{SystemOrder::kNormal}, recorder, 2);
        scheduler.AddSystem<TagSystem>(SystemPhase::Update, SystemOrder{SystemOrder::kEarly}, recorder, 3);
        scheduler.AddSystem<TagSystem>(SystemPhase::Update, SystemOrder{SystemOrder::kNormal}, recorder, 4);
        scheduler.Start(context);
        scheduler.Update(context);
        return recorder.updated;
    };

    REQUIRE(run(SystemTies{}) == std::vector<int>{3, 1, 2, 4});
    REQUIRE(run(SystemTies{SystemTies::Reverse}) == std::vector<int>{3, 4, 2, 1});
}

TEST_CASE("A shuffle seed repeats its order, and only ties move", "[ecs][system]")
{
    World world;
    CommandBuffer commands;
    SystemContext context{world, commands};

    const auto run = [&](uint32_t seed)
    {
        Recorder recorder;
        SystemScheduler scheduler{SystemTies{SystemTies::Shuffle, seed}};
        scheduler.AddSystem<TagSystem>(SystemPhase::Update, SystemOrder{SystemOrder::kLate}, recorder, 0);
        for (int id = 1; id <= 8; ++id)
            scheduler.AddSystem<TagSystem>(SystemPhase::Update, SystemOrder{SystemOrder::kNormal}, recorder, id);
        scheduler.AddSystem<TagSystem>(SystemPhase::Update, SystemOrder{SystemOrder::kEarly}, recorder, 9);
        scheduler.Start(context);
        scheduler.Update(context);
        return recorder.updated;
    };

    const std::vector<int> registration{9, 1, 2, 3, 4, 5, 6, 7, 8, 0};
    bool moved = false;
    for (uint32_t seed = 0; seed < 4; ++seed)
    {
        const std::vector<int> order = run(seed);
        REQUIRE(order == run(seed));
        REQUIRE(order.front() == 9);
        REQUIRE(order.back() == 0);
        REQUIRE(std::is_permutation(order.begin(), order.end(), registration.begin()));
        moved = moved || order != registration;
    }
    REQUIRE(moved);
}

TEST_CASE("SystemNames lists a phase's systems in run order", "[ecs][system]")
{
    SystemScheduler scheduler{SystemTies{}};
    scheduler.AddSystem<CountSystem>(SystemPhase::Update, SystemOrder{SystemOrder::kLate});
    scheduler.AddSystem<SpawnSystem>(SystemPhase::Update, SystemOrder{SystemOrder::kEarly});

    REQUIRE(scheduler.SystemNames(SystemPhase::Update) == std::vector<std::string_view>{"SpawnSystem", "CountSystem"});
    REQUIRE(scheduler.SystemNames(SystemPhase::Render).empty());
}
