/**
 * @file SceneIO.h
 * @author Sumin Park
 * @brief Save and load a scene as a manifest plus one file per entity.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once
#include "core/ecs/Entity.h"
#include "scene/SceneAsset.h"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace mir
{
    class World;

    inline constexpr std::string_view kSceneManifestFileName = "scene.json";

    // The manifest SaveScene writes and LoadScene reads. Callers test for it to
    // decide whether a directory holds a scene at all.
    std::filesystem::path SceneManifestPath(const std::filesystem::path &sceneDir);

    class ScenePrefabResolver
    {
    public:
        virtual ~ScenePrefabResolver() = default;

        // An empty map means the prefab is unavailable.
        virtual std::map<SceneNodeAddress, Entity> Materialize(World &world, const ScenePrefabInstance &instance) = 0;

        // False when the prefab is unavailable or the node, component or field
        // is not an EntityRef of the template.
        virtual bool TemplateEntityRef(const ScenePrefabInstance &instance, const SceneNodeAddress &node,
            std::string_view component, std::string_view field,
            std::optional<SceneNodeAddress> &outTarget) const = 0;
    };

    // The root's Transform fields as an instance record's placement; empty when it has no Transform.
    nlohmann::json InstancePlacementFromWorld(World &world, Entity root);

    bool SaveScene(World &world, const std::filesystem::path &sceneDir, const LoadedScene &scene,
        const ScenePrefabResolver *resolver = nullptr);

    LoadedScene LoadScene(World &world, const std::filesystem::path &sceneDir,
        ScenePrefabResolver *resolver = nullptr);

    // Destroy all entity in scene
    void UnloadScene(World &world, const LoadedScene &loaded);
}
