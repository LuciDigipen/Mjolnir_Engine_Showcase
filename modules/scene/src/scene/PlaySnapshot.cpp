/**
 * @file PlaySnapshot.cpp
 * @author Sumin Park
 * @brief  Serialization of the world
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "scene/PlaySnapshot.h"

#include "core/ecs/World.h"
#include "core/log/SetupError.h"
#include "core/platform/Process.h"
#include "scene/SceneIO.h"

#include <algorithm>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace mir
{
    namespace
    {
        std::filesystem::path SnapshotDir()
        {
            return std::filesystem::temp_directory_path() /
                ("mjolnir_play_snapshot_" + std::to_string(CurrentProcessId()));
        }

        // Above every id the scene holds or has handed out, so a snapshot-only id
        // can never collide with a saved one.
        StableId HighWaterMark(const LoadedScene &scene)
        {
            StableId next = scene.mNextId;
            for (const auto &entry : scene.mEntities)
                next = std::max(next, entry.first + 1);
            for (const auto &[root, instance] : scene.mInstances)
            {
                next = std::max(next, root + 1);
                for (const auto &binding : instance.mBindings)
                    next = std::max(next, binding.second + 1);
            }
            return next;
        }

        LoadedScene TrackAllEntities(World &world, std::string name)
        {
            LoadedScene scene = NewScene(std::move(name));
            world.ForEachAliveEntity([&](Entity entity)
                {
                    scene.mEntities[AllocateStableId(scene)] = entity;
                });
            return scene;
        }
    }

    PlaySnapshot CaptureWorld(World &world, const LoadedScene &scene, const ScenePrefabResolver *resolver)
    {
        PlaySnapshot snapshot;
        snapshot.mDir = SnapshotDir();
        snapshot.mNextId = scene.mNextId;
        snapshot.mDirty = scene.mDirty;

        std::error_code ec;
        std::filesystem::remove_all(snapshot.mDir, ec);

        LoadedScene copy = scene;
        copy.mNextId = HighWaterMark(scene);

        std::unordered_set<uint64_t> tracked;
        tracked.reserve(scene.mEntities.size());
        for (const auto &entry : scene.mEntities)
            tracked.insert(PackEntity(entry.second));

        world.ForEachAliveEntity([&](Entity entity)
            {
                if (tracked.contains(PackEntity(entity)))
                    return;

                const StableId id = AllocateStableId(copy);
                copy.mEntities[id] = entity;
                snapshot.mUntrackedIds.push_back(id);
            });

        if (!SaveScene(world, snapshot.mDir, copy, resolver))
        {
            std::filesystem::remove_all(snapshot.mDir, ec);
            throw SetupError(ErrorCategory::System, "Could not copy the live scene into Play", snapshot.mDir,
                "the temporary scene could not be written",
                "Check free space and write permissions on the temporary directory.");
        }

        return snapshot;
    }

    PlaySnapshot CaptureWorld(World &world)
    {
        return CaptureWorld(world, TrackAllEntities(world, "__play_snapshot"));
    }

    LoadedScene RestoreWorld(World &world, const PlaySnapshot &snapshot, ScenePrefabResolver *resolver)
    {
        std::vector<Entity> alive;
        world.ForEachAliveEntity([&](Entity entity)
            {
                alive.push_back(entity);
            });

        for (Entity entity : alive)
        {
            if (world.IsAlive(entity))
                world.DestroyEntity(entity);
        }

        LoadedScene restored = LoadScene(world, snapshot.mDir, resolver);
        for (StableId id : snapshot.mUntrackedIds)
            restored.mEntities.erase(id);
        restored.mNextId = snapshot.mNextId;
        restored.mDirty = snapshot.mDirty;
        return restored;
    }

    LoadedScene CopyWorld(World &source, const LoadedScene &scene, World &destination,
        const ScenePrefabResolver *sourceResolver, ScenePrefabResolver *destinationResolver)
    {
        PlaySnapshot snapshot = CaptureWorld(source, scene, sourceResolver);
        try
        {
            LoadedScene copy = RestoreWorld(destination, snapshot, destinationResolver);
            DiscardSnapshot(snapshot);
            return copy;
        }
        catch (...)
        {
            DiscardSnapshot(snapshot);
            throw;
        }
    }

    void DiscardSnapshot(PlaySnapshot &snapshot)
    {
        if (snapshot.mDir.empty())
            return;

        std::error_code ec;
        std::filesystem::remove_all(snapshot.mDir, ec);
        snapshot.mDir.clear();
    }
}
