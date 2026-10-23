/**
 * @file PrefabInstantiate.tests.cpp
 * @author Jackey Cen
 * @brief Tests for prefab/PrefabInstantiate.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <prefab/PrefabInstantiate.h>

#include <core/ecs/ComponentRegistry.h>
#include <core/ecs/TransformHierarchy.h>
#include <core/ecs/World.h>
#include <core/ecs/components/Transform.h>
#include <core/math/WorldReal.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace
{
    using nlohmann::json;
    using mir::CompiledPrefab;
    using mir::ComponentOps;
    using mir::ComponentRegistry;
    using mir::Entity;
    using mir::FieldKind;
    using mir::HierarchyIndex;
    using mir::PrefabCompileResult;
    using mir::PrefabComponentInstance;
    using mir::PrefabDocument;
    using mir::PrefabInstantiation;
    using mir::PrefabNodeAddress;
    using mir::PrefabNodeId;
    using mir::PrefabParseResult;
    using mir::RuntimeFieldDecl;
    using mir::Transform;
    using mir::World;
    using mir::WorldVec3;

    const char *kUuid = "11111111-2222-4333-8444-555555555555";

    const ComponentOps &LinkOps()
    {
        static const RuntimeFieldDecl fields[] = {
            { "target", FieldKind::EntityRef },
            { "speed", FieldKind::Float },
        };
        return ComponentRegistry::Instance().RegisterRuntime("PrefabInstantiateTestLink", fields);
    }

    const ComponentOps &TagOps()
    {
        static const std::span<const RuntimeFieldDecl> noFields;
        return ComponentRegistry::Instance().RegisterRuntime("PrefabInstantiateTestTag", noFields);
    }

    PrefabDocument ParseDocument(const json &root)
    {
        const PrefabParseResult parsed = mir::ParsePrefab(root);
        REQUIRE_FALSE(mir::HasPrefabErrors(parsed.mDiagnostics));
        REQUIRE(parsed.mDocument.has_value());
        return *parsed.mDocument;
    }

    json InstantiateDocumentJson()
    {
        json root;
        root["formatVersion"] = 1;
        root["prefabId"] = kUuid;
        root["nextNodeId"] = "5";
        root["rootNode"] = "1";
        root["nodes"] = json::array({
            json{
                { "id", "1" },
                { "kind", "entity" },
                { "components", json::array({
                    json{ { "type", "Transform" }, { "fields", json{
                        { "position", json::array({ 1.0, 2.0, 3.0 }) },
                    } } },
                    json{ { "type", "PrefabInstantiateTestLink" }, { "fields", json{
                        { "target", json::array({ "3" }) },
                        { "speed", 2.0 },
                    } } },
                    json{ { "type", "PrefabInstantiateTestTag" } },
                }) },
            },
            json{
                { "id", "2" },
                { "kind", "entity" },
                { "parent", "1" },
                { "components", json::array({
                    json{ { "type", "Transform" }, { "fields", json{
                        { "position", json::array({ 2.0, 0.0, 0.0 }) },
                    } } },
                }) },
            },
            json{
                { "id", "3" },
                { "kind", "entity" },
                { "parent", "1" },
                { "components", json::array({
                    json{ { "type", "Transform" }, { "fields", json{
                        { "position", json::array({ 3.0, 0.0, 0.0 }) },
                    } } },
                }) },
            },
            json{
                { "id", "4" },
                { "kind", "entity" },
                { "parent", "2" },
                { "components", json::array({
                    json{ { "type", "Transform" }, { "fields", json{
                        { "position", json::array({ 0.0, 4.0, 0.0 }) },
                    } } },
                }) },
            },
        });
        return root;
    }

    const CompiledPrefab &CompileDocument()
    {
        static const PrefabDocument doc = ParseDocument(InstantiateDocumentJson());
        static const PrefabCompileResult result = mir::CompilePrefab(doc, mir::PrefabDocumentLookup{});
        REQUIRE(result.mPrefab != nullptr);
        return *result.mPrefab;
    }

    Entity TargetOf(World &world, Entity entity)
    {
        const ComponentOps &ops = LinkOps();
        void *component = ops.GetComponent(world, entity);
        REQUIRE(component != nullptr);
        Entity target;
        target.mIndex = 0;
        ops.FindField("target")->Read(component, &target);
        return target;
    }

    float SpeedOf(World &world, Entity entity)
    {
        const ComponentOps &ops = LinkOps();
        void *component = ops.GetComponent(world, entity);
        REQUIRE(component != nullptr);
        float speed = 0.0f;
        ops.FindField("speed")->Read(component, &speed);
        return speed;
    }

    void SetSpeed(World &world, Entity entity, float speed)
    {
        const ComponentOps &ops = LinkOps();
        void *component = ops.GetComponent(world, entity);
        REQUIRE(component != nullptr);
        ops.FindField("speed")->Write(component, &speed);
    }

    std::vector<Entity> ChildrenOf(World &world, Entity parent)
    {
        const HierarchyIndex &index = world.HierarchyIndex();
        const std::span<const Entity> children = index.ChildrenOf(parent);
        return std::vector<Entity>(children.begin(), children.end());
    }

    Entity EntityAt(const PrefabInstantiation &instantiation, PrefabNodeId id)
    {
        const auto it = instantiation.mEntities.find(PrefabNodeAddress{ id });
        REQUIRE(it != instantiation.mEntities.end());
        return it->second;
    }
}

TEST_CASE("InstantiatePrefab rebuilds hierarchy, sibling order and internal references", "[prefab]")
{
    mir::RegisterCoreComponents();
    LinkOps();
    TagOps();

    const CompiledPrefab &prefab = CompileDocument();
    World world;
    const PrefabInstantiation instantiation = mir::InstantiatePrefab(world, prefab);

    REQUIRE(world.IsAlive(instantiation.mRoot));
    CHECK(instantiation.mRoot == EntityAt(instantiation, 1));
    CHECK(instantiation.mEntities.size() == prefab.Nodes().size());

    const Entity root = EntityAt(instantiation, 1);
    const Entity first = EntityAt(instantiation, 2);
    const Entity second = EntityAt(instantiation, 3);
    const Entity grandchild = EntityAt(instantiation, 4);

    CHECK(mir::ParentOf(world, first) == root);
    CHECK(mir::ParentOf(world, second) == root);
    CHECK(mir::ParentOf(world, grandchild) == first);
    CHECK(ChildrenOf(world, root) == std::vector<Entity>{ first, second });

    REQUIRE(world.GetComponent<Transform>(root) != nullptr);
    CHECK(world.GetComponent<Transform>(root)->Position() == WorldVec3(1.0, 2.0, 3.0));
    CHECK(world.GetComponent<Transform>(first)->Position() == WorldVec3(2.0, 0.0, 0.0));
    CHECK(world.GetComponent<Transform>(second)->Position() == WorldVec3(3.0, 0.0, 0.0));
    CHECK(world.GetComponent<Transform>(grandchild)->Position() == WorldVec3(0.0, 4.0, 0.0));

    CHECK(TargetOf(world, root) == second);
    CHECK(SpeedOf(world, root) == 2.0f);
    CHECK(TagOps().Has(world, root));
}

TEST_CASE("Prefab instances are independent", "[prefab]")
{
    mir::RegisterCoreComponents();
    LinkOps();
    TagOps();

    const CompiledPrefab &prefab = CompileDocument();
    World world;
    const PrefabInstantiation first = mir::InstantiatePrefab(world, prefab);
    const PrefabInstantiation second = mir::InstantiatePrefab(world, prefab);

    CHECK(first.mRoot != second.mRoot);
    for (PrefabNodeId id : { PrefabNodeId{ 1 }, PrefabNodeId{ 2 }, PrefabNodeId{ 3 }, PrefabNodeId{ 4 } })
        CHECK(EntityAt(first, id) != EntityAt(second, id));

    CHECK(TargetOf(world, first.mRoot) == EntityAt(first, 3));
    CHECK(TargetOf(world, second.mRoot) == EntityAt(second, 3));

    world.GetComponent<Transform>(first.mRoot)->SetPosition(WorldVec3(9.0, 9.0, 9.0));
    SetSpeed(world, first.mRoot, 8.0f);

    CHECK(world.GetComponent<Transform>(second.mRoot)->Position() == WorldVec3(1.0, 2.0, 3.0));
    CHECK(SpeedOf(world, second.mRoot) == 2.0f);

    const PrefabInstantiation third = mir::InstantiatePrefab(world, prefab);
    CHECK(world.GetComponent<Transform>(third.mRoot)->Position() == WorldVec3(1.0, 2.0, 3.0));
    CHECK(SpeedOf(world, third.mRoot) == 2.0f);
    CHECK(TargetOf(world, third.mRoot) == EntityAt(third, 3));
}

TEST_CASE("InstantiatePrefab appends the root under the given parent", "[prefab]")
{
    mir::RegisterCoreComponents();
    LinkOps();
    TagOps();

    const CompiledPrefab &prefab = CompileDocument();
    World world;
    const Entity parent = world.CreateEntity();
    const Entity existing = world.CreateEntity();
    REQUIRE(mir::SetParent(world, existing, parent, mir::kNullEntity, false));

    const PrefabInstantiation instantiation = mir::InstantiatePrefab(world, prefab, parent);

    CHECK(mir::ParentOf(world, instantiation.mRoot) == parent);
    CHECK(ChildrenOf(world, parent) == std::vector<Entity>{ existing, instantiation.mRoot });
    CHECK(ChildrenOf(world, instantiation.mRoot) ==
        std::vector<Entity>{ EntityAt(instantiation, 2), EntityAt(instantiation, 3) });
}

TEST_CASE("InstantiatePrefab keeps authored far positions exact", "[prefab]")
{
    if (!mir::kLargeWorld)
        SKIP("exact far positions need a double-precision world");

    mir::RegisterCoreComponents();

    PrefabDocument doc = mir::NewPrefabDocument();
    doc.mNodes[0].mComponents.push_back(PrefabComponentInstance{
        "Transform", json{ { "position", json::array({ 1000000000.5, -2000000000.25, 3000000000.125 }) } } });

    const PrefabCompileResult compiled = mir::CompilePrefab(doc, mir::PrefabDocumentLookup{});
    REQUIRE(compiled.mPrefab != nullptr);

    World world;
    const PrefabInstantiation instantiation = mir::InstantiatePrefab(world, *compiled.mPrefab);

    const WorldVec3 expected(static_cast<mir::WorldReal>(1000000000.5),
        static_cast<mir::WorldReal>(-2000000000.25), static_cast<mir::WorldReal>(3000000000.125));
    CHECK(world.GetComponent<Transform>(instantiation.mRoot)->Position() == expected);
}
