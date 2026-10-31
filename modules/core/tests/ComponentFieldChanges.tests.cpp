/**
 * @file ComponentFieldChanges.tests.cpp
 * @author Jackey Cen
 * @brief Tests for core/ecs/ComponentFieldChanges.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <core/ecs/ComponentFieldChanges.h>
#include <core/ecs/World.h>

#include <catch2/catch_test_macros.hpp>

#include <span>
#include <string>
#include <vector>

namespace
{
    using mir::ComponentFieldChange;
    using mir::ComponentFieldListeners;
    using mir::Entity;
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

    void RecordBefore(World &world, std::span<const ComponentFieldChange> changes, void *user)
    {
        auto &recorder = *static_cast<Recorder *>(user);
        recorder.mCalls.push_back("before");
        for (const ComponentFieldChange &change : changes)
        {
            const Health *health = world.GetComponent<Health>(change.mEntity);
            if (health != nullptr)
                recorder.mValues.push_back(health->mValue);
        }
    }

    void RecordAfter(World &world, std::span<const ComponentFieldChange> changes, void *user)
    {
        RecordBefore(world, changes, user);
        static_cast<Recorder *>(user)->mCalls.back() = "after";
    }

    std::vector<ComponentFieldChange> ChangedHealth(Entity entity)
    {
        return { ComponentFieldChange{ entity, TypeIdOf<Health>(), { "mValue" } } };
    }
}

TEST_CASE("Component field listeners run in registration order and removal takes one out", "[ecs][changes]")
{
    World world;
    ComponentFieldListeners listeners;
    Recorder first;
    Recorder second;

    listeners.AddBeforeChange(&RecordBefore, &first);
    listeners.AddBeforeChange(&RecordBefore, &second);

    const Entity entity = world.CreateEntity();
    world.AddComponent<Health>(entity, Health{ 3 });

    listeners.NotifyBeforeChange(world, ChangedHealth(entity));
    REQUIRE(first.mCalls == std::vector<std::string>{ "before" });
    REQUIRE(second.mCalls == std::vector<std::string>{ "before" });

    listeners.RemoveBeforeChange(&RecordBefore, &first);
    listeners.NotifyBeforeChange(world, ChangedHealth(entity));
    REQUIRE(first.mCalls == std::vector<std::string>{ "before" });
    REQUIRE(second.mCalls == std::vector<std::string>{ "before", "before" });
}

TEST_CASE("BeforeChange sees the components a commit is about to change", "[ecs][changes]")
{
    World world;
    ComponentFieldListeners listeners;
    Recorder recorder;
    listeners.AddBeforeChange(&RecordBefore, &recorder);
    listeners.AddAfterChange(&RecordAfter, &recorder);

    const Entity entity = world.CreateEntity();
    world.AddComponent<Health>(entity, Health{ 10 });
    const std::vector<ComponentFieldChange> changes = ChangedHealth(entity);

    listeners.NotifyBeforeChange(world, changes);
    REQUIRE(recorder.mCalls == std::vector<std::string>{ "before" });
    REQUIRE(recorder.mValues == std::vector<int>{ 10 });

    world.GetComponent<Health>(entity)->mValue = 20;
    listeners.NotifyAfterChange(world, changes);
    REQUIRE(recorder.mCalls == std::vector<std::string>{ "before", "after" });
    REQUIRE(recorder.mValues == std::vector<int>{ 10, 20 });
}

TEST_CASE("A listener added twice runs once", "[ecs][changes]")
{
    World world;
    ComponentFieldListeners listeners;
    Recorder recorder;
    listeners.AddBeforeChange(&RecordBefore, &recorder);
    listeners.AddBeforeChange(&RecordBefore, &recorder);

    const Entity entity = world.CreateEntity();
    listeners.NotifyBeforeChange(world, ChangedHealth(entity));
    REQUIRE(recorder.mCalls == std::vector<std::string>{ "before" });
}
