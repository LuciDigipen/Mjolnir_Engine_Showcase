/**
 * @file PlaySnapshot.h
 * @author Sumin Park
 * @brief  Serialization of the world
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once
#include "scene/SceneAsset.h"

#include <filesystem>
#include <vector>

namespace mir
{
    class World;
    class ScenePrefabResolver;

    struct PlaySnapshot
    {
        std::filesystem::path mDir;

        // Entities the played scene did not track; they stay outside it after a restore.
        std::vector<StableId> mUntrackedIds;

        // The played scene's own values, which the snapshot's ids must not disturb.
        StableId mNextId = 1;
        bool mDirty = false;
    };

    // Captures the world with the scene that is being played.
    PlaySnapshot CaptureWorld(World &world, const LoadedScene &scene,
        const ScenePrefabResolver *resolver = nullptr);

    // Whole-World form for callers with no editor scene.
    PlaySnapshot CaptureWorld(World &world);

    LoadedScene RestoreWorld(World &world, const PlaySnapshot &snapshot,
        ScenePrefabResolver *resolver = nullptr);

    // Seeds another World from the current scene without changing the source World.
    LoadedScene CopyWorld(World &source, const LoadedScene &scene, World &destination,
        const ScenePrefabResolver *sourceResolver = nullptr, ScenePrefabResolver *destinationResolver = nullptr);

    // Removes the snapshot's directory, one per process, once it will not be
    // restored again; the snapshot is empty afterwards
    void DiscardSnapshot(PlaySnapshot &snapshot);
}
