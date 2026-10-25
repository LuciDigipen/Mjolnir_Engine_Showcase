/**
 * @file WorldSceneOps.cpp
 * @author Dong Ta
 * @brief The scene calls the editor's history makes, done on a World and its LoadedScene.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <editor/WorldSceneOps.h>

#include <editor/PrefabWorkspace.h>

#include <core/ecs/World.h>
#include <core/log/Log.h>
#include <prefab/PrefabId.h>
#include <scene/EntitySnapshot.h>
#include <scene/SceneAsset.h>
#include <scene/SceneDuplicate.h>
#include <scene/SceneIO.h>

#include <optional>
#include <string>
#include <unordered_map>

namespace mir
{
    Entity WorldSceneOps::CreateEntity()
    {
        return CreateSceneEntity(mWorld, mScene);
    }

    Entity WorldSceneOps::Duplicate(Entity source)
    {
        return DuplicateSceneEntitySubtree(mWorld, mScene, source);
    }

    Entity WorldSceneOps::CopyAsOrdinaryObject(Entity source)
    {
        return CopyInstanceMemberAsOrdinary(mWorld, mScene, source);
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

        subtree.mInstances = CaptureInstanceRecords(mScene, subtree.mStableIds);

        return subtree;
    }

    std::vector<Entity> WorldSceneOps::Restore(const CapturedSubtree &subtree, Entity parent, Entity before)
    {
        std::vector<Entity> members = RestoreEntityAt(mWorld, EntitySnapshot{ subtree.mData }, mScene,
            subtree.mStableIds, parent, before);
        if (!members.empty() && !subtree.mInstances.empty() && !RestoreInstanceRecords(mScene, subtree.mInstances))
            MIR_LOG_WARN("WorldSceneOps: could not restore a prefab instance record");
        return members;
    }

    bool WorldSceneOps::Destroy(Entity root)
    {
        StableId instanceRoot = kNullStableId;
        const std::optional<StableId> rootId = StableIdOf(mScene, root);
        if (rootId && FindInstanceBinding(mScene, *rootId, instanceRoot) != nullptr && instanceRoot != *rootId)
        {
            MIR_LOG_WARN("WorldSceneOps: cannot delete a prefab instance member; delete its root instead");
            return false;
        }

        return DestroySceneEntity(mWorld, mScene, root);
    }

    Entity WorldSceneOps::PlacePrefab(std::string_view contentPath, Entity parent, const WorldVec3 &position)
    {
        const std::optional<PrefabId> id =
            mPrefabs.FindByPath(mProjectRoot / std::filesystem::path(std::string(contentPath)));
        if (!id)
        {
            MIR_LOG_ERROR("Place Prefab: '{}' is not a registered Prefab", contentPath);
            return kNullEntity;
        }

        return mPrefabs.Place(mWorld, *id, parent, position);
    }

    bool WorldSceneOps::CanReparent(std::span<const Entity> entities, Entity parent)
    {
        StableId instanceRoot = kNullStableId;
        if (!parent.IsNull())
        {
            const std::optional<StableId> parentId = StableIdOf(mScene, parent);
            if (parentId && FindInstanceBinding(mScene, *parentId, instanceRoot) != nullptr)
            {
                MIR_LOG_WARN("WorldSceneOps: cannot reparent under a prefab instance member");
                return false;
            }
        }

        for (const Entity entity : entities)
        {
            const std::optional<StableId> id = StableIdOf(mScene, entity);
            if (id && FindInstanceBinding(mScene, *id, instanceRoot) != nullptr && instanceRoot != *id)
            {
                MIR_LOG_WARN("WorldSceneOps: cannot reparent a prefab instance member");
                return false;
            }
        }
        return true;
    }
}
