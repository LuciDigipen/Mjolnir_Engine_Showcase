/**
 * @file WorldSceneOps.cpp
 * @author Dong Ta
 * @brief The scene calls the editor's history makes, done on a World and its LoadedScene.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <editor/WorldSceneOps.h>

#include <core/ecs/World.h>
#include <scene/EntitySnapshot.h>
#include <scene/SceneAsset.h>

#include <unordered_map>

namespace mir
{
    Entity WorldSceneOps::CreateEntity()
    {
        return CreateSceneEntity(mWorld, mScene);
    }

    Entity WorldSceneOps::Duplicate(Entity source)
    {
        return DuplicateSceneEntity(mWorld, mScene, source);
    }

    CapturedSubtree WorldSceneOps::Capture(Entity root)
    {
        World &world = mWorld;

        CapturedSubtree subtree;
        subtree.mMembers = SnapshotMembers(world, root);
        subtree.mData = CaptureEntity(world, root).mData;

        std::unordered_map<uint64_t, StableId> ids;
        for (const auto &[id, entity] : mScene.mEntities)
            ids[PackEntity(entity)] = id;

        subtree.mStableIds.reserve(subtree.mMembers.size());
        for (const Entity member : subtree.mMembers)
        {
            const auto it = ids.find(PackEntity(member));
            subtree.mStableIds.push_back(it != ids.end() ? it->second : kNullStableId);
        }
        return subtree;
    }

    std::vector<Entity> WorldSceneOps::Restore(const CapturedSubtree &subtree, Entity parent, Entity before)
    {
        std::vector<Entity> members = RestoreEntityAt(mWorld, EntitySnapshot{ subtree.mData }, mScene,
            subtree.mStableIds, parent, before);
        return members;
    }

    bool WorldSceneOps::Destroy(Entity root)
    {
        return DestroySceneEntity(mWorld, mScene, root);
    }
}
