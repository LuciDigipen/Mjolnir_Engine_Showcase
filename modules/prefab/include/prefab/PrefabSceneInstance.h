/**
 * @file PrefabSceneInstance.h
 * @author Jackey Cen
 * @brief Scene prefab resolver and placement built on the compiled prefab graph.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once
#include "prefab/CompiledPrefab.h"
#include "prefab/PrefabRegistry.h"
#include "scene/SceneAsset.h"
#include "scene/SceneIO.h"

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string_view>

namespace mir
{
    class PrefabSceneResolver final : public ScenePrefabResolver
    {
    public:
        PrefabSceneResolver(const PrefabRegistry &registry, CompiledPrefabCache &cache);

        std::map<SceneNodeAddress, Entity> Materialize(World &world, const ScenePrefabInstance &instance) override;

        bool TemplateNodes(const ScenePrefabInstance &instance, std::set<SceneNodeAddress> &outNodes) const override;

        bool TemplateEntityRef(const ScenePrefabInstance &instance, const SceneNodeAddress &node,
            std::string_view component, std::string_view field,
            std::optional<SceneNodeAddress> &outTarget) const override;

    private:
        std::shared_ptr<const CompiledPrefab> FindOrCompile(const ScenePrefabInstance &instance) const;

        const PrefabRegistry &mRegistry;
        CompiledPrefabCache &mCache;
    };

    // Registers every member under a fresh StableId and records the instance.
    StableId PlacePrefabInstance(World &world, LoadedScene &scene, const CompiledPrefab &prefab,
        Entity parent = kNullEntity);
}
