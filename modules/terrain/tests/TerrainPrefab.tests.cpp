/**
 * @file TerrainPrefab.tests.cpp
 * @author Jackey Cen
 * @brief Tests for terrain component prefab capture.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <terrain/ComponentRegistration.h>
#include <terrain/components/TerrainInstance.h>

#include <core/ecs/ComponentRegistry.h>
#include <core/ecs/TransformHierarchy.h>
#include <core/ecs/World.h>
#include <core/ecs/components/Transform.h>
#include <prefab/PrefabCapture.h>
#include <prefab/PrefabFormat.h>
#include <scene/SceneAsset.h>

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <string>
#include <string_view>

using namespace mir;

namespace
{
    const PrefabComponentInstance *FindComponent(const PrefabNode &node, std::string_view type)
    {
        for (const PrefabComponentInstance &component : node.mComponents)
            if (component.mType == type)
                return &component;
        return nullptr;
    }
}

TEST_CASE("A TerrainInstance material binding survives prefab capture", "[terrain][prefab]")
{
    RegisterCoreComponents();
    RegisterTerrainComponents();
    World world;
    LoadedScene scene = NewScene("prefab");
    const Entity root = CreateSceneEntity(world, scene);
    AddTransform(world, root, Transform{});

    TerrainInstance instance{};
    constexpr std::string_view recipe = "terrain/relief.terrain.json";
    constexpr std::string_view material = "materials/icelandicSand.material.json";
    std::memcpy(instance.recipePath, recipe.data(), recipe.size());
    std::memcpy(instance.materialPath, material.data(), material.size());
    world.AddComponent(root, instance);

    const PrefabCaptureResult captured = CapturePrefab(world, scene, root);
    INFO(captured.mRefusal);
    REQUIRE(captured.mDocument.has_value());
    REQUIRE(captured.mDocument->mNodes.size() == 1);
    const PrefabComponentInstance *terrain = FindComponent(captured.mDocument->mNodes[0], "TerrainInstance");
    REQUIRE(terrain != nullptr);
    CHECK(terrain->mFields.at("recipePath").get<std::string>() == recipe);
    CHECK(terrain->mFields.at("materialPath").get<std::string>() == material);
}
