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

    bool SaveScene(World &world, const std::filesystem::path &sceneDir, const LoadedScene &scene);

    // Scene error checker
    struct SceneProbe
    {
        // non-empty: scene.json cannot be read as a scene, so LoadScene would give an empty one
        std::string mManifestError;

        // listed in scene.json, but the entity file is missing or unreadable
        std::vector<StableId> mMissingEntities;

        bool Ok() const { return mManifestError.empty() && mMissingEntities.empty(); }
    };

    // Reads scene.json and every entity file it lists; call before unloading what LoadScene replaces
    SceneProbe ProbeScene(const std::filesystem::path &sceneDir);

    LoadedScene LoadScene(World &world, const std::filesystem::path &sceneDir);

    // Destroy all entity in scene
    void UnloadScene(World &world, const LoadedScene &loaded);
}
