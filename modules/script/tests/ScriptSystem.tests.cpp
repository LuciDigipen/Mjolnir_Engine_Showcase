/**
 * @file ScriptSystem.tests.cpp
 * @author Jackey Cen
 * @brief Tests for script/ScriptSystem.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <script/ComponentRegistration.h>
#include <script/IScriptHost.h>
#include <script/ScriptSystem.h>

#include <core/ecs/CommandBuffer.h>
#include <core/ecs/ComponentFieldChanges.h>
#include <core/ecs/ComponentRegistry.h>
#include <core/ecs/World.h>

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <string_view>
#include <vector>

namespace
{
    using mir::CommandBuffer;
    using mir::ComponentFieldChange;
    using mir::ComponentFieldListeners;
    using mir::Entity;
    using mir::IScriptHost;
    using mir::ScriptRef;
    using mir::ScriptSystem;
    using mir::SystemContext;
    using mir::TypeIdOf;
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
        void CallOnFixedUpdate(World &, CommandBuffer &, Entity, int32_t, float) override {}
        void CallOnStop(World &, CommandBuffer &, Entity, int32_t) override {}

        std::vector<int32_t> mDestroyed;

    private:
        int32_t mNextInstance = 0;
    };

    // the instance is attached after the add, the way the resolver does it
    Entity Scripted(World &world, int32_t instanceRef)
    {
        ScriptRef ref;
        std::strncpy(ref.scriptName, "spin", sizeof(ref.scriptName) - 1);

        const Entity entity = world.CreateEntity();
        ScriptRef &added = world.AddComponent<ScriptRef>(entity, ref);
        added.instanceRef = instanceRef;
        added.started = true;
        return entity;
    }
}

TEST_CASE("BeforeChange releases only a changed script name", "[script][changes]")
{
    mir::RegisterCoreComponents();
    mir::RegisterScriptComponents();

    World world;
    CommandBuffer commands;
    world.EmplaceResource<ComponentFieldListeners>();
    FakeHost host;
    ScriptSystem system(host);

    SystemContext context{ world, commands };
    system.OnStart(context);

    const Entity entity = Scripted(world, 8);
    ComponentFieldListeners &listeners = world.Resource<ComponentFieldListeners>();

    const std::vector<ComponentFieldChange> unrelated{ { entity, TypeIdOf<ScriptRef>(), { "speed" } } };
    listeners.NotifyBeforeChange(world, unrelated);
    CHECK(host.mDestroyed.empty());

    const std::vector<ComponentFieldChange> named{ { entity, TypeIdOf<ScriptRef>(), { "scriptName" } } };
    listeners.NotifyBeforeChange(world, named);
    REQUIRE(host.mDestroyed == std::vector<int32_t>{ 8 });
    const ScriptRef *ref = world.GetComponent<ScriptRef>(entity);
    REQUIRE(ref != nullptr);
    CHECK(ref->instanceRef == -1);
    CHECK_FALSE(ref->started);

    system.OnStop(context);
}

TEST_CASE("OnStop unsubscribes from field changes", "[script][changes]")
{
    mir::RegisterCoreComponents();
    mir::RegisterScriptComponents();

    World world;
    CommandBuffer commands;
    world.EmplaceResource<ComponentFieldListeners>();
    FakeHost host;
    ScriptSystem system(host);

    SystemContext context{ world, commands };
    system.OnStart(context);

    const Entity entity = Scripted(world, 9);

    system.OnStop(context);

    const std::vector<ComponentFieldChange> changes{ { entity, TypeIdOf<ScriptRef>(), { "scriptName" } } };
    world.Resource<ComponentFieldListeners>().NotifyBeforeChange(world, changes);
    CHECK(host.mDestroyed.empty());
    CHECK(world.GetComponent<ScriptRef>(entity)->instanceRef == 9);
}
