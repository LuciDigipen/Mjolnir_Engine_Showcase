/**
 * @file AppSceneOps.cpp
 * @author Dong Ta
 * @brief The scene calls the editor's history makes, done on App's World and LoadedScene.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "AppSceneOps.h"

#include <app/App.h>
#include <scene/EntitySnapshot.h>
#include <scene/SceneAsset.h>

#include <unordered_map>

namespace mir
{
    Entity AppSceneOps::CreateEntity()
    {
        return CreateSceneEntity(mApp.GetWorld(), mApp.Scene());
    }

    Entity AppSceneOps::Duplicate(Entity source)
    {
        return DuplicateSceneEntity(mApp.GetWorld(), mApp.Scene(), source);
    }

    CapturedSubtree AppSceneOps::Capture(Entity root)
    {
        World &world = mApp.GetWorld();

        CapturedSubtree subtree;
        subtree.mMembers = SnapshotMembers(world, root);
        subtree.mData = CaptureEntity(world, root).mData;

        std::unordered_map<uint64_t, StableId> ids;
        for (const auto &[id, entity] : mApp.Scene().mEntities)
            ids[PackEntity(entity)] = id;

        subtree.mStableIds.reserve(subtree.mMembers.size());
        for (const Entity member : subtree.mMembers)
        {
            const auto it = ids.find(PackEntity(member));
            subtree.mStableIds.push_back(it != ids.end() ? it->second : kNullStableId);
        }
        return subtree;
    }

    std::vector<Entity> AppSceneOps::Restore(const CapturedSubtree &subtree, Entity parent, Entity before)
    {
        std::vector<Entity> members = RestoreEntityAt(mApp.GetWorld(), EntitySnapshot{ subtree.mData }, mApp.Scene(),
            subtree.mStableIds, parent, before);
        return members;
    }

    bool AppSceneOps::Destroy(Entity root)
    {
        return DestroySceneEntity(mApp.GetWorld(), mApp.Scene(), root);
    }
}
