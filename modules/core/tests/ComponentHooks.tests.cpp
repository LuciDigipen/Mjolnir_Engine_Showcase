/**
 * @file ComponentHooks.tests.cpp
 * @author sumin.park
 * @brief Tests for World's per-type OnAdd and OnRemove hooks.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <core/ecs/TransformHierarchy.h>
#include <core/ecs/World.h>

#include <catch2/catch_test_macros.hpp>

#include <stdexcept>
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

    // the neighbour has HVelocity too, so its own destroy fires this hook again
    world.SetComponentHooks<HVelocity>({ .onRemove = [&neighbours, entity](World &w, Entity e)
        {
            if (e == entity)
                w.DestroyEntity(neighbours[0]);
        } });
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

TEST_CASE("DestroyEntity fires OnRemove for every component, with all the data still readable", "[ecs][hooks][destroy]")
{
    // each hook reads the other component, so neither may have been dropped yet
    World world;
    float positionSeenVelocity = -1.0f;
    float velocitySeenPosition = -1.0f;
    world.SetComponentHooks<HPosition>({ .onRemove = [&](World &w, Entity e) { positionSeenVelocity = w.GetComponent<HVelocity>(e)->dx; } });
    world.SetComponentHooks<HVelocity>({ .onRemove = [&](World &w, Entity e) { velocitySeenPosition = w.GetComponent<HPosition>(e)->x; } });

    const Entity entity = world.CreateEntity();
    world.AddComponent(entity, HPosition{ 1.0f });
    world.AddComponent(entity, HVelocity{ 2.0f });
    world.DestroyEntity(entity);

    CHECK(positionSeenVelocity == 2.0f);
    CHECK(velocitySeenPosition == 1.0f);
    CHECK_FALSE(world.IsAlive(entity));
}

TEST_CASE("DestroyEntity fires OnRemove for tags", "[ecs][hooks][destroy]")
{
    World world;
    int removes = 0;
    world.SetComponentHooks<HFrozen>({ .onRemove = [&removes](World &, Entity) { ++removes; } });

    const Entity entity = world.CreateEntity();
    world.AddTag<HFrozen>(entity);
    world.DestroyEntity(entity);

    CHECK(removes == 1);
}

TEST_CASE("DestroyEntity fires a child's OnRemove before its parent's", "[ecs][hooks][destroy]")
{
    World world;
    std::vector<Entity> order;
    world.SetComponentHooks<HPosition>({ .onRemove = [&order](World &, Entity e) { order.push_back(e); } });

    const Entity parent = world.CreateEntity();
    const Entity child = world.CreateEntity();
    world.AddComponent(parent, HPosition{});
    world.AddComponent(child, HPosition{});
    REQUIRE(mir::SetParent(world, child, parent));

    world.DestroyEntity(parent);

    CHECK(order == std::vector<Entity>{ child, parent });
}

TEST_CASE("A child's OnRemove still reads its parent during the cascade", "[ecs][hooks][destroy]")
{
    World world;
    Entity seenParent = mir::kNullEntity;
    world.SetComponentHooks<HVelocity>({ .onRemove = [&seenParent](World &w, Entity e) { seenParent = mir::ParentOf(w, e); } });

    const Entity parent = world.CreateEntity();
    const Entity child = world.CreateEntity();
    world.AddComponent(child, HVelocity{});
    REQUIRE(mir::SetParent(world, child, parent));

    world.DestroyEntity(parent);

    CHECK(seenParent == parent);
}

TEST_CASE("An OnRemove fired by DestroyEntity may destroy another entity", "[ecs][hooks][destroy]")
{
    World world;
    const Entity other = world.CreateEntity();
    world.AddComponent(other, HVelocity{});
    world.SetComponentHooks<HPosition>({ .onRemove = [other](World &w, Entity) { w.DestroyEntity(other); } });

    const Entity entity = world.CreateEntity();
    world.AddComponent(entity, HPosition{});
    world.DestroyEntity(entity);

    CHECK_FALSE(world.IsAlive(other));
    CHECK_FALSE(world.IsAlive(entity));
}

TEST_CASE("An OnRemove in a cascade may destroy a sibling", "[ecs][hooks][destroy]")
{
    World world;
    const Entity parent = world.CreateEntity();
    const Entity a = world.CreateEntity();
    const Entity b = world.CreateEntity();
    world.AddComponent(a, HPosition{});
    REQUIRE(mir::SetParent(world, a, parent));
    REQUIRE(mir::SetParent(world, b, parent));
    world.SetComponentHooks<HPosition>({ .onRemove = [b](World &w, Entity)
        {
            if (w.IsAlive(b))
                w.DestroyEntity(b);
        } });

    world.DestroyEntity(parent);

    CHECK_FALSE(world.IsAlive(parent));
    CHECK_FALSE(world.IsAlive(a));
    CHECK_FALSE(world.IsAlive(b));
    CHECK(world.HierarchyIndex().Roots().empty());
}

TEST_CASE("A child whose OnRemove destroys its parent leaves no hang and a clean graph", "[ecs][hooks][destroy]")
{
    World world;
    const Entity parent = world.CreateEntity();
    const Entity first = world.CreateEntity();
    const Entity killer = world.CreateEntity();
    const Entity last = world.CreateEntity();
    for (const Entity child : { first, killer, last })
        REQUIRE(mir::SetParent(world, child, parent));

    world.AddComponent(killer, HPosition{});
    world.SetComponentHooks<HPosition>({ .onRemove = [parent](World &w, Entity) { w.DestroyEntity(parent); } });

    world.DestroyEntity(killer);

    CHECK_FALSE(world.IsAlive(parent));
    CHECK_FALSE(world.IsAlive(first));
    CHECK_FALSE(world.IsAlive(killer));
    CHECK_FALSE(world.IsAlive(last));
    CHECK(world.HierarchyIndex().Roots().empty());

    // the reused slots start clean: one root, no stale links
    const Entity fresh = world.CreateEntity();
    CHECK(world.HierarchyIndex().Roots().size() == 1);
    CHECK(world.HierarchyIndex().ChildrenOf(fresh).empty());
}

TEST_CASE("A child a hook adds to the dying parent is destroyed too", "[ecs][hooks][destroy]")
{
    World world;
    const Entity parent = world.CreateEntity();
    const Entity child = world.CreateEntity();
    world.AddComponent(child, HPosition{});
    REQUIRE(mir::SetParent(world, child, parent));

    Entity added = mir::kNullEntity;
    world.SetComponentHooks<HPosition>({ .onRemove = [parent, &added](World &w, Entity)
        {
            added = w.CreateEntity();
            mir::SetParent(w, added, parent);
        } });

    world.DestroyEntity(parent);

    REQUIRE_FALSE(added.IsNull());
    CHECK_FALSE(world.IsAlive(added));
    CHECK(world.HierarchyIndex().Roots().empty());
}

TEST_CASE("An entity being destroyed cannot be attached", "[ecs][hooks][destroy]")
{
    // SetParent asserts on it; CanSetParent gives the same answer without asserting
    World world;
    const Entity other = world.CreateEntity();
    const Entity entity = world.CreateEntity();
    world.AddComponent(entity, HPosition{});

    bool attachable = true;
    world.SetComponentHooks<HPosition>({ .onRemove = [other, &attachable](World &w, Entity e)
        {
            attachable = mir::CanSetParent(w, e, other);
        } });

    world.DestroyEntity(entity);

    CHECK_FALSE(attachable);
}

TEST_CASE("Two hooks destroying each other's entity end with both destroyed", "[ecs][hooks][destroy]")
{
    World world;
    const Entity a = world.CreateEntity();
    const Entity b = world.CreateEntity();
    world.AddComponent(a, HPosition{});
    world.AddComponent(b, HPosition{});
    world.SetComponentHooks<HPosition>({ .onRemove = [a, b](World &w, Entity e)
        {
            const Entity partner = e == a ? b : a;
            if (w.IsAlive(partner))
                w.DestroyEntity(partner);
        } });

    world.DestroyEntity(a);

    CHECK_FALSE(world.IsAlive(a));
    CHECK_FALSE(world.IsAlive(b));
}

TEST_CASE("A hook destroying its own entity mid-destroy is ignored", "[ecs][hooks][destroy]")
{
    World world;
    int removes = 0;
    world.SetComponentHooks<HPosition>({ .onRemove = [&removes](World &w, Entity e)
        {
            ++removes;
            w.DestroyEntity(e);
        } });

    const Entity entity = world.CreateEntity();
    world.AddComponent(entity, HPosition{});
    world.DestroyEntity(entity);

    CHECK(removes == 1);
    CHECK_FALSE(world.IsAlive(entity));
}

TEST_CASE("A hook that throws mid-destroy leaves the entity destroyable", "[ecs][hooks][destroy]")
{
    World world;
    bool thrown = false;
    world.SetComponentHooks<HPosition>({ .onRemove = [&thrown](World &, Entity)
        {
            if (!thrown)
            {
                thrown = true;
                throw std::runtime_error("hook failed");
            }
        } });

    const Entity entity = world.CreateEntity();
    world.AddComponent(entity, HPosition{});
    REQUIRE_THROWS_AS(world.DestroyEntity(entity), std::runtime_error);
    REQUIRE(world.IsAlive(entity));

    world.DestroyEntity(entity);

    CHECK_FALSE(world.IsAlive(entity));
}
