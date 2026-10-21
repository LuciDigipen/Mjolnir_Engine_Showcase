/**
 * @file EntityChanges.tests.cpp
 * @author Jackey Cen
 * @brief Tests for core/ecs/EntityChanges.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <core/ecs/EntityChanges.h>
#include <core/ecs/World.h>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

namespace
{
    using mir::Entity;
    using mir::EntityChangeListeners;
    using mir::EntityChangeSet;
    using mir::EntityComponentChange;
    using mir::EntityComponentRef;
    using mir::TypeIdOf;
    using mir::World;

    struct Health
    {
        int mValue = 0;
    };

    struct Recorder
    {
        std::vector<std::string> mCalls;
        std::vector<int> mValues;
    };

    void RecordBefore(World &world, const EntityChangeSet &changes, void *user)
    {
        auto &recorder = *static_cast<Recorder *>(user);
        recorder.mCalls.push_back("before");
        for (Entity entity : changes.mRemovedEntities)
        {
            const Health *health = world.GetComponent<Health>(entity);
            if (health != nullptr)
                recorder.mValues.push_back(health->mValue);
        }
        for (const EntityComponentChange &change : changes.mChangedComponents)
        {
            const Health *health = world.GetComponent<Health>(change.mEntity);
            if (health != nullptr)
                recorder.mValues.push_back(health->mValue);
        }
    }

    void RecordAfter(World &world, const EntityChangeSet &changes, void *user)
    {
        RecordBefore(world, changes, user);
        static_cast<Recorder *>(user)->mCalls.back() = "after";
    }

    EntityChangeSet ChangedHealth(Entity entity)
    {
        EntityChangeSet changes;
        changes.mChangedComponents.push_back(EntityComponentChange{ entity, TypeIdOf<Health>(), { "mValue" } });
        return changes;
    }
}

TEST_CASE("Entity change listeners run in registration order and removal takes one out", "[ecs][changes]")
{
    World world;
    EntityChangeListeners listeners;
    Recorder first;
    Recorder second;

    listeners.AddBeforeRemove(&RecordBefore, &first);
    listeners.AddBeforeRemove(&RecordBefore, &second);

    const Entity entity = world.CreateEntity();
    world.AddComponent<Health>(entity, Health{ 3 });

    listeners.NotifyBeforeRemove(world, ChangedHealth(entity));
    REQUIRE(first.mCalls == std::vector<std::string>{ "before" });
    REQUIRE(second.mCalls == std::vector<std::string>{ "before" });

    listeners.RemoveBeforeRemove(&RecordBefore, &first);
    listeners.NotifyBeforeRemove(world, ChangedHealth(entity));
    REQUIRE(first.mCalls == std::vector<std::string>{ "before" });
    REQUIRE(second.mCalls == std::vector<std::string>{ "before", "before" });
}

TEST_CASE("BeforeRemove sees the components a commit is about to change", "[ecs][changes]")
{
    World world;
    EntityChangeListeners listeners;
    Recorder recorder;
    listeners.AddBeforeRemove(&RecordBefore, &recorder);
    listeners.AddAfterChange(&RecordAfter, &recorder);

    const Entity entity = world.CreateEntity();
    world.AddComponent<Health>(entity, Health{ 10 });
    const EntityChangeSet changes = ChangedHealth(entity);

    listeners.NotifyBeforeRemove(world, changes);
    REQUIRE(recorder.mCalls == std::vector<std::string>{ "before" });
    REQUIRE(recorder.mValues == std::vector<int>{ 10 });

    world.GetComponent<Health>(entity)->mValue = 20;
    listeners.NotifyAfterChange(world, changes);
    REQUIRE(recorder.mCalls == std::vector<std::string>{ "before", "after" });
    REQUIRE(recorder.mValues == std::vector<int>{ 10, 20 });
}

TEST_CASE("A change set keeps entity and component entries apart", "[ecs][changes]")
{
    World world;
    const Entity entity = world.CreateEntity();

    EntityChangeSet changes;
    changes.mRemovedEntities.push_back(entity);
    changes.mRemovedComponents.push_back(EntityComponentRef{ entity, TypeIdOf<Health>() });
    changes.mAddedEntities.push_back(entity);
    changes.mAddedComponents.push_back(EntityComponentRef{ entity, TypeIdOf<Health>() });

    REQUIRE(changes.mRemovedEntities.size() == 1);
    REQUIRE(changes.mRemovedComponents.size() == 1);
    REQUIRE(changes.mAddedEntities.size() == 1);
    REQUIRE(changes.mAddedComponents.size() == 1);
}
