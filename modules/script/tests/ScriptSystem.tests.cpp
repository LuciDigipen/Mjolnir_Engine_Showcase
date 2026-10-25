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
#include <core/ecs/ComponentRegistry.h>
#include <core/ecs/EntityChanges.h>
#include <core/ecs/World.h>

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <string_view>
#include <vector>

namespace
{
    using mir::CommandBuffer;
    using mir::Entity;
    using mir::EntityChangeListeners;
    using mir::EntityChangeSet;
    using mir::EntityComponentChange;
    using mir::EntityComponentRef;
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

    ScriptRef MakeRef(int32_t instanceRef)
    {
        ScriptRef ref;
        ref.instanceRef = instanceRef;
        ref.started = true;
        std::strncpy(ref.scriptName, "spin", sizeof(ref.scriptName) - 1);
        return ref;
    }
}

TEST_CASE("BeforeRemove releases a script binding that a commit removes", "[script][changes]")
{
    mir::RegisterCoreComponents();
    mir::RegisterScriptComponents();

    World world;
    CommandBuffer commands;
    world.EmplaceResource<EntityChangeListeners>();
    FakeHost host;
    ScriptSystem system(host);

    SystemContext context{ world, commands };
    system.OnStart(context);

    const Entity entity = world.CreateEntity();
    world.AddComponent<ScriptRef>(entity, MakeRef(7));

    EntityChangeSet changes;
    changes.mRemovedComponents.push_back(EntityComponentRef{ entity, TypeIdOf<ScriptRef>() });
    world.Resource<EntityChangeListeners>().NotifyBeforeRemove(world, changes);

    REQUIRE(host.mDestroyed == std::vector<int32_t>{ 7 });
    const ScriptRef *ref = world.GetComponent<ScriptRef>(entity);
    REQUIRE(ref != nullptr);
    CHECK(ref->instanceRef == -1);
    CHECK_FALSE(ref->started);

    system.OnStop(context);
}

TEST_CASE("BeforeRemove releases only a changed script name", "[script][changes]")
{
    mir::RegisterCoreComponents();
    mir::RegisterScriptComponents();

    World world;
    CommandBuffer commands;
    world.EmplaceResource<EntityChangeListeners>();
    FakeHost host;
    ScriptSystem system(host);

    SystemContext context{ world, commands };
    system.OnStart(context);

    const Entity entity = world.CreateEntity();
    world.AddComponent<ScriptRef>(entity, MakeRef(8));
    EntityChangeListeners &listeners = world.Resource<EntityChangeListeners>();

    EntityChangeSet unrelated;
    unrelated.mChangedComponents.push_back(EntityComponentChange{ entity, TypeIdOf<ScriptRef>(), { "speed" } });
    listeners.NotifyBeforeRemove(world, unrelated);
    CHECK(host.mDestroyed.empty());

    EntityChangeSet named;
    named.mChangedComponents.push_back(EntityComponentChange{ entity, TypeIdOf<ScriptRef>(), { "scriptName" } });
    listeners.NotifyBeforeRemove(world, named);
    REQUIRE(host.mDestroyed == std::vector<int32_t>{ 8 });
    CHECK(world.GetComponent<ScriptRef>(entity)->instanceRef == -1);

    system.OnStop(context);
}

TEST_CASE("OnStop unsubscribes from entity changes", "[script][changes]")
{
    mir::RegisterCoreComponents();
    mir::RegisterScriptComponents();

    World world;
    CommandBuffer commands;
    world.EmplaceResource<EntityChangeListeners>();
    FakeHost host;
    ScriptSystem system(host);

    SystemContext context{ world, commands };
    system.OnStart(context);

    const Entity entity = world.CreateEntity();
    world.AddComponent<ScriptRef>(entity, MakeRef(9));

    system.OnStop(context);

    EntityChangeSet changes;
    changes.mRemovedEntities.push_back(entity);
    world.Resource<EntityChangeListeners>().NotifyBeforeRemove(world, changes);
    CHECK(host.mDestroyed.empty());
    CHECK(world.GetComponent<ScriptRef>(entity)->instanceRef == 9);
}
