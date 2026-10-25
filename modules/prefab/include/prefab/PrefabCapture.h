/**
 * @file PrefabCapture.h
 * @author Jackey Cen
 * @brief Captures an ordinary scene subtree into a new Prefab document.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "prefab/PrefabDocument.h"
#include "prefab/PrefabRegistry.h"

#include <core/ecs/Entity.h>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace mir
{
    class World;
    struct LoadedScene;

    struct PrefabCaptureResult
    {
        std::optional<PrefabDocument> mDocument;
        std::string mRefusal;
    };

    // A refused capture leaves the World, the scene and the registry untouched.
    PrefabCaptureResult CapturePrefab(World &world, const LoadedScene &scene, Entity root);

    // A file name with the characters Windows does not allow replaced.
    std::string PrefabNameForFile(std::string_view name);

    // The first free "<name>.prefab" in prefabsDir, suffixed when the path is taken.
    std::filesystem::path UniquePrefabPath(const std::filesystem::path &prefabsDir, std::string_view name,
        const PrefabRegistry &registry);
}
