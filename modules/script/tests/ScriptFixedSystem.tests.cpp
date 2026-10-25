/**
 * @file ScriptFixedSystem.tests.cpp
 * @author sumin.park
 * @brief Tests for script/ScriptFixedSystem: OnFixedUpdate once per fixed step.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <script/ComponentRegistration.h>
#include <script/IScriptHost.h>
#include <script/ScriptFixedSystem.h>

#include <core/ecs/CommandBuffer.h>
#include <core/ecs/ComponentRegistry.h>
#include <core/ecs/EngineMode.h>
#include <core/ecs/SystemScheduler.h>
#include <core/ecs/World.h>

#include <catch2/catch_test_macros.hpp>

#include <stdexcept>
#include <string_view>
#include <vector>

namespace
{
    using mir::CommandBuffer;
    using mir::Entity;
    using mir::ScriptRef;
    using mir::World;

    constexpr float kStep = 1.0f / 60.0f;

    struct FixedCall
    {
        Entity entity;
        int32_t instanceRef;
        float dt;
    };

    class FakeHost final : public mir::IScriptHost
    {
    public:
        bool LoadScriptSource(std::string_view, std::string_view) override { return true; }
        bool ReloadScriptSource(std::string_view, std::string_view) override { return true; }
        void UnloadScript(std::string_view) override {}

        int32_t CreateInstance(std::string_view) override { return 0; }
        void DestroyInstance(int32_t) override {}

        void CallOnStart(World &, CommandBuffer &, Entity, int32_t) override {}
        void CallOnUpdate(World &, CommandBuffer &, Entity, int32_t, float) override {}
        void CallOnFixedUpdate(World &, CommandBuffer &, Entity entity, int32_t instanceRef, float dt) override
        {
            if (mThrowOn == instanceRef)
                throw std::runtime_error("script failed");
            mCalls.push_back({ entity, instanceRef, dt });
        }
        void CallOnStop(World &, CommandBuffer &, Entity, int32_t) override {}

        std::vector<FixedCall> mCalls;
        int32_t mThrowOn = -1;
    };

    struct Fixture
    {
        World world;
        CommandBuffer commands;
        mir::SystemScheduler scheduler;
        FakeHost host;
        mir::SystemContext context{ world, commands, kStep, 0.0, 0 };

        explicit Fixture(mir::EngineMode mode = mir::EngineMode::Play)
        {
            mir::RegisterCoreComponents();
            mir::RegisterScriptComponents();
            world.EmplaceResource<mir::EngineModeState>();
            mir::SetEngineMode(world, mode);
            scheduler.AddSystem<mir::ScriptFixedSystem>(
                mir::SystemPhase::FixedUpdate, mir::SystemOrder{ mir::SystemOrder::kEarly }, host);
            scheduler.Start(context);
        }

        ~Fixture() { scheduler.Stop(context); }

        Entity Scripted(int32_t instanceRef, bool started = true)
        {
            const Entity entity = world.CreateEntity();
            world.AddComponent<ScriptRef>(entity, ScriptRef{});
            ScriptRef *ref = world.GetComponent<ScriptRef>(entity);
            ref->instanceRef = instanceRef;
            ref->started = started;
            return entity;
        }
    };
}

TEST_CASE("OnFixedUpdate runs once per fixed step and never on a frame with no step", "[script][fixed]")
{
    Fixture f;
    f.Scripted(1);

    f.scheduler.Update(f.context, 1, kStep);
    CHECK(f.host.mCalls.size() == 1);

    f.scheduler.Update(f.context, 0, kStep);
    CHECK(f.host.mCalls.size() == 1);

    // a frame that catches up three steps calls it three times
    f.scheduler.Update(f.context, 3, kStep);
    CHECK(f.host.mCalls.size() == 4);
}

TEST_CASE("OnFixedUpdate gets the fixed step as dt", "[script][fixed]")
{
    Fixture f;
    f.Scripted(1);

    f.context.dt = 0.5f;
    f.scheduler.Update(f.context, 1, kStep);

    REQUIRE(f.host.mCalls.size() == 1);
    CHECK(f.host.mCalls[0].dt == kStep);
}

TEST_CASE("OnFixedUpdate waits until OnStart has run", "[script][fixed]")
{
    Fixture f;
    const Entity waiting = f.Scripted(1, false);
    f.Scripted(2);

    f.scheduler.Update(f.context, 1, kStep);
    REQUIRE(f.host.mCalls.size() == 1);
    CHECK(f.host.mCalls[0].instanceRef == 2);

    f.world.GetComponent<ScriptRef>(waiting)->started = true;
    f.scheduler.Update(f.context, 1, kStep);
    CHECK(f.host.mCalls.size() == 3);
}

TEST_CASE("OnFixedUpdate skips a ScriptRef with no instance and does nothing outside Play", "[script][fixed]")
{
    Fixture f;
    f.Scripted(-1);
    f.scheduler.Update(f.context, 1, kStep);
    CHECK(f.host.mCalls.empty());

    Fixture editing(mir::EngineMode::Edit);
    editing.Scripted(1);
    editing.scheduler.Update(editing.context, 1, kStep);
    CHECK(editing.host.mCalls.empty());
}

TEST_CASE("A script that throws in OnFixedUpdate does not stop the others", "[script][fixed]")
{
    Fixture f;
    f.host.mThrowOn = 1;
    f.Scripted(1);
    f.Scripted(2);

    f.scheduler.Update(f.context, 1, kStep);

    REQUIRE(f.host.mCalls.size() == 1);
    CHECK(f.host.mCalls[0].instanceRef == 2);
}
