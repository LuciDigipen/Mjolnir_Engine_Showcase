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
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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

        // Every node address of the instance's compiled template. False when the
        // prefab is unavailable or did not compile, which confirms nothing about
        // an address the template might have dropped.
        virtual bool TemplateNodes(const ScenePrefabInstance &instance, std::set<SceneNodeAddress> &outNodes) const = 0;

        // False when the prefab is unavailable or the node, component or field
        // is not an EntityRef of the template.
        virtual bool TemplateEntityRef(const ScenePrefabInstance &instance, const SceneNodeAddress &node,
            std::string_view component, std::string_view field,
            std::optional<SceneNodeAddress> &outTarget) const = 0;
    };

    // The root's Transform fields as an instance record's placement; empty when it has no Transform.
    nlohmann::json InstancePlacementFromWorld(World &world, Entity root);

    // The prefab instance record rooted at root as JSON text; empty when the scene has none.
    std::string CaptureInstanceRecord(const LoadedScene &scene, StableId root);

    // Puts a captured instance record back under its root id; false when the text is malformed.
    bool RestoreInstanceRecord(LoadedScene &scene, std::string_view text);

    // Every instance record whose root is one of members, as JSON text.
    std::vector<std::string> CaptureInstanceRecords(const LoadedScene &scene, std::span<const StableId> members);

    // Puts every record back; false when one text is malformed.
    bool RestoreInstanceRecords(LoadedScene &scene, std::span<const std::string> records);

    bool SaveScene(World &world, const std::filesystem::path &sceneDir, const LoadedScene &scene,
        const ScenePrefabResolver *resolver = nullptr);

    LoadedScene LoadScene(World &world, const std::filesystem::path &sceneDir,
        ScenePrefabResolver *resolver = nullptr);

    // Destroy all entity in scene
    void UnloadScene(World &world, const LoadedScene &loaded);
}
