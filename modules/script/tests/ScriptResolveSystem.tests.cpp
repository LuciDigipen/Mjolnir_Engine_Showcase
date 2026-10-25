/**
 * @file ScriptResolveSystem.tests.cpp
 * @author sumin.park
 * @brief Tests for ScriptResolveSystem: resolve, retry after a failed load, rename.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <script/ComponentRegistration.h>
#include <script/IScriptHost.h>
#include <script/ScriptResolveSystem.h>

#include <core/ecs/CommandBuffer.h>
#include <core/ecs/ComponentRegistry.h>
#include <core/ecs/World.h>

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    using mir::CommandBuffer;
    using mir::Entity;
    using mir::IScriptHost;
    using mir::ScriptRef;
    using mir::ScriptResolveSystem;
    using mir::SystemContext;
    using mir::World;

    class FakeHost final : public IScriptHost
    {
    public:
        bool LoadScriptSource(std::string_view, std::string_view) override { return true; }
        bool ReloadScriptSource(std::string_view, std::string_view) override { return true; }
        void UnloadScript(std::string_view) override {}

        int32_t CreateInstance(std::string_view) override { return mNextInstance++; }
        void DestroyInstance(int32_t instanceRef) override { mDestroyed.push_back(instanceRef); }

        void CallOnStart(World &, CommandBuffer &, Entity, int32_t) override {}
        void CallOnUpdate(World &, CommandBuffer &, Entity, int32_t, float) override {}
        void CallOnStop(World &, CommandBuffer &, Entity, int32_t) override {}

        std::vector<int32_t> mDestroyed;

    private:
        int32_t mNextInstance = 0;
    };

    ScriptRef Named(const char *name)
    {
        ScriptRef ref;
        std::strncpy(ref.scriptName, name, sizeof(ref.scriptName) - 1);
        return ref;
    }

    struct Fixture
    {
        World world;
        CommandBuffer commands;
        FakeHost host;
        std::vector<std::string> loads;
        bool loadSucceeds = true;
        ScriptResolveSystem system{ host, [this](std::string_view name)
            {
                loads.emplace_back(name);
                return loadSucceeds;
            } };
        SystemContext context{ world, commands };

        Fixture()
        {
            mir::RegisterCoreComponents();
            mir::RegisterScriptComponents();
            system.OnStart(context);
        }
    };
}

TEST_CASE("A ScriptRef gets an instance without Play, its script loaded once", "[script][resolve]")
{
    Fixture f;
    const Entity a = f.world.CreateEntity();
    const Entity b = f.world.CreateEntity();
    f.world.AddComponent(a, Named("Door"));
    f.world.AddComponent(b, Named("Door"));

    f.system.OnUpdate(f.context);

    CHECK(f.world.GetComponent<ScriptRef>(a)->instanceRef >= 0);
    CHECK(f.world.GetComponent<ScriptRef>(b)->instanceRef >= 0);
    CHECK(f.world.GetComponent<ScriptRef>(a)->instanceRef != f.world.GetComponent<ScriptRef>(b)->instanceRef);
    CHECK(f.loads == std::vector<std::string>{ "Door" });
}

TEST_CASE("A script that fails to load is retried only after ForgetFailures", "[script][resolve]")
{
    Fixture f;
    f.loadSucceeds = false;
    const Entity entity = f.world.CreateEntity();
    f.world.AddComponent(entity, Named("Missing"));

    f.system.OnUpdate(f.context);
    f.system.OnUpdate(f.context);
    CHECK(f.loads.size() == 1);
    CHECK(f.world.GetComponent<ScriptRef>(entity)->instanceRef < 0);

    f.loadSucceeds = true;
    f.system.ForgetFailures();
    f.system.OnUpdate(f.context);
    CHECK(f.loads.size() == 2);
    CHECK(f.world.GetComponent<ScriptRef>(entity)->instanceRef >= 0);
}

TEST_CASE("Renaming a ScriptRef replaces its instance", "[script][resolve]")
{
    Fixture f;
    const Entity entity = f.world.CreateEntity();
    f.world.AddComponent(entity, Named("Door"));
    f.system.OnUpdate(f.context);
    const int32_t before = f.world.GetComponent<ScriptRef>(entity)->instanceRef;

    ScriptRef &ref = *f.world.GetComponent<ScriptRef>(entity);
    ref.started = true;
    std::strncpy(ref.scriptName, "Lamp", sizeof(ref.scriptName) - 1);
    f.system.OnUpdate(f.context);

    CHECK(f.host.mDestroyed == std::vector<int32_t>{ before });
    CHECK(ref.instanceRef >= 0);
    CHECK(ref.instanceRef != before);
    CHECK_FALSE(ref.started);
}

TEST_CASE("A copy made with no instance gets its own, and outlives the original", "[script][resolve]")
{
    // Duplicate and snapshots never copy instanceRef, so a copy arrives unresolved
    Fixture f;
    const Entity original = f.world.CreateEntity();
    f.world.AddComponent(original, Named("Door"));
    f.system.OnUpdate(f.context);

    const Entity copy = f.world.CreateEntity();
    f.world.AddComponent(copy, Named("Door"));
    f.world.DestroyEntity(original);
    f.system.OnUpdate(f.context);

    CHECK(f.world.GetComponent<ScriptRef>(copy)->instanceRef >= 0);
}
