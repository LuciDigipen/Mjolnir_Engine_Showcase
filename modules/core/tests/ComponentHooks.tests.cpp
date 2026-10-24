/**
 * @file ComponentHooks.tests.cpp
 * @author sumin.park
 * @brief Tests for World's per-type OnAdd and OnRemove hooks.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <core/ecs/World.h>

#include <catch2/catch_test_macros.hpp>

#include <vector>

namespace
{
    using mir::ComponentHooks;
    using mir::Entity;
    using mir::World;

    struct HPosition
    {
        float x = 0.0f;
    };

    struct HVelocity
    {
        float dx = 0.0f;
    };

    struct HFrozen
    {
    };
}

TEST_CASE("OnAdd runs after the value is stored", "[ecs][hooks]")
{
    World world;
    float seen = -1.0f;
    world.SetComponentHooks<HPosition>({ .onAdd = [&seen](World &w, Entity e) { seen = w.GetComponent<HPosition>(e)->x; } });

    const Entity entity = world.CreateEntity();
    world.AddComponent(entity, HPosition{ 3.0f });

    CHECK(seen == 3.0f);
}

TEST_CASE("OnRemove runs before the data is dropped", "[ecs][hooks]")
{
    World world;
    float seen = -1.0f;
    world.SetComponentHooks<HPosition>({ .onRemove = [&seen](World &w, Entity e) { seen = w.GetComponent<HPosition>(e)->x; } });

    const Entity entity = world.CreateEntity();
    world.AddComponent(entity, HPosition{ 5.0f });
    world.RemoveComponent<HPosition>(entity);

    CHECK(seen == 5.0f);
    CHECK_FALSE(world.Has<HPosition>(entity));
}

TEST_CASE("Adding a component the entity already has fires no OnAdd", "[ecs][hooks]")
{
    World world;
    int adds = 0;
    world.SetComponentHooks<HPosition>({ .onAdd = [&adds](World &, Entity) { ++adds; } });

    const Entity entity = world.CreateEntity();
    world.AddComponent(entity, HPosition{});
    world.AddComponent(entity, HPosition{});

    CHECK(adds == 1);
}

TEST_CASE("Hooks fire on the type-erased paths", "[ecs][hooks]")
{
    World world;
    int adds = 0;
    int removes = 0;
    world.SetComponentHooks<HPosition>({ .onAdd = [&adds](World &, Entity) { ++adds; },
        .onRemove = [&removes](World &, Entity) { ++removes; } });

    const Entity entity = world.CreateEntity();
    const HPosition value{ 1.0f };
    world.AddRaw(entity, mir::TypeIdOf<HPosition>(), sizeof(HPosition), alignof(HPosition), &value);
    world.RemoveRaw(entity, mir::TypeIdOf<HPosition>());

    CHECK(adds == 1);
    CHECK(removes == 1);
}

TEST_CASE("Hooks fire for tags", "[ecs][hooks]")
{
    World world;
    int adds = 0;
    int removes = 0;
    world.SetComponentHooks<HFrozen>({ .onAdd = [&adds](World &, Entity) { ++adds; },
        .onRemove = [&removes](World &, Entity) { ++removes; } });

    const Entity entity = world.CreateEntity();
    world.AddTag<HFrozen>(entity);
    world.RemoveComponent<HFrozen>(entity);

    CHECK(adds == 1);
    CHECK(removes == 1);
}

TEST_CASE("OnAdd may add a companion, and AddComponent still returns the live value", "[ecs][hooks]")
{
    World world;
    world.SetComponentHooks<HPosition>({ .onAdd = [](World &w, Entity e) { w.AddComponent(e, HVelocity{ 2.0f }); } });

    const Entity entity = world.CreateEntity();
    HPosition &added = world.AddComponent(entity, HPosition{ 1.0f });
    added.x = 9.0f;

    REQUIRE(world.Has<HVelocity>(entity));
    CHECK(world.GetComponent<HVelocity>(entity)->dx == 2.0f);
    CHECK(world.GetComponent<HPosition>(entity)->x == 9.0f);
}

TEST_CASE("OnRemove may destroy another entity", "[ecs][hooks]")
{
    World world;
    const Entity other = world.CreateEntity();
    world.SetComponentHooks<HPosition>({ .onRemove = [other](World &w, Entity) { w.DestroyEntity(other); } });

    const Entity entity = world.CreateEntity();
    world.AddComponent(entity, HPosition{ 4.0f });
    world.RemoveComponent<HPosition>(entity);

    CHECK_FALSE(world.IsAlive(other));
    CHECK(world.IsAlive(entity));
    CHECK_FALSE(world.Has<HPosition>(entity));
}

TEST_CASE("Removing from a shared table keeps the other entities' data", "[ecs][hooks]")
{
    // destroying the first row swaps the removed entity, the last row, into its place
    World world;
    std::vector<Entity> neighbours;
    for (int i = 0; i < 3; ++i)
    {
        neighbours.push_back(world.CreateEntity());
        world.AddComponent(neighbours.back(), HPosition{ static_cast<float>(i) });
        world.AddComponent(neighbours.back(), HVelocity{});
    }

    const Entity entity = world.CreateEntity();
    world.AddComponent(entity, HPosition{ 7.0f });
    world.AddComponent(entity, HVelocity{ 8.0f });

    world.SetComponentHooks<HVelocity>({ .onRemove = [&neighbours](World &w, Entity) { w.DestroyEntity(neighbours[0]); } });
    world.RemoveComponent<HVelocity>(entity);

    CHECK(world.GetComponent<HPosition>(entity)->x == 7.0f);
    CHECK(world.GetComponent<HPosition>(neighbours[1])->x == 1.0f);
    CHECK(world.GetComponent<HPosition>(neighbours[2])->x == 2.0f);
}

TEST_CASE("ClearComponentHooks stops the hooks, and new ones can be set after", "[ecs][hooks]")
{
    World world;
    int first = 0;
    int second = 0;
    world.SetComponentHooks<HPosition>({ .onAdd = [&first](World &, Entity) { ++first; } });
    world.ClearComponentHooks<HPosition>();

    const Entity entity = world.CreateEntity();
    world.AddComponent(entity, HPosition{});
    CHECK(first == 0);

    world.SetComponentHooks<HPosition>({ .onAdd = [&second](World &, Entity) { ++second; } });
    world.AddComponent(world.CreateEntity(), HPosition{});
    CHECK(second == 1);
}

TEST_CASE("Hooks belong to one World", "[ecs][hooks]")
{
    World hooked;
    World plain;
    int adds = 0;
    hooked.SetComponentHooks<HPosition>({ .onAdd = [&adds](World &, Entity) { ++adds; } });

    plain.AddComponent(plain.CreateEntity(), HPosition{});

    CHECK(adds == 0);
}
