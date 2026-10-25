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
#include <core/log/Log.h>
#include <scene/EntitySnapshot.h>
#include <scene/SceneAsset.h>
#include <scene/SceneDuplicate.h>
#include <scene/SceneIO.h>

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
        StableId rootId = kNullStableId;
        for (const auto &[id, entity] : mScene.mEntities)
        {
            if (entity == root)
            {
                rootId = id;
                break;
            }
        }

        if (rootId != kNullStableId)
        {
            for (const auto &[instanceRoot, instance] : mScene.mInstances)
            {
                bool member = false;
                for (const auto &[address, id] : instance.mBindings)
                {
                    (void)address;
                    member |= id == rootId;
                }
                if (!member)
                    continue;

                if (instanceRoot == rootId)
                    break;

                MIR_LOG_WARN("WorldSceneOps: cannot delete a prefab instance member; delete its root instead");
                return false;
            }
        }

        return DestroySceneEntity(mWorld, mScene, root);
    }

    bool WorldSceneOps::CanReparent(std::span<const Entity> entities, Entity parent)
    {
        const LoadedScene &scene = mScene;
        const auto stableIdOf = [&scene](Entity entity)
            {
                for (const auto &[id, candidate] : scene.mEntities)
                {
                    if (candidate == entity)
                        return id;
                }
                return kNullStableId;
            };
        const auto memberState = [&scene](StableId id, bool &isRoot)
            {
                for (const auto &[instanceRoot, instance] : scene.mInstances)
                {
                    for (const auto &[address, member] : instance.mBindings)
                    {
                        (void)address;
                        if (member != id)
                            continue;
                        isRoot = instanceRoot == id;
                        return true;
                    }
                }
                return false;
            };

        if (!parent.IsNull())
        {
            bool parentIsRoot = false;
            const StableId parentId = stableIdOf(parent);
            if (parentId != kNullStableId && memberState(parentId, parentIsRoot))
            {
                MIR_LOG_WARN("WorldSceneOps: cannot reparent under a prefab instance member");
                return false;
            }
        }

        for (const Entity entity : entities)
        {
            bool isRoot = false;
            const StableId id = stableIdOf(entity);
            if (id != kNullStableId && memberState(id, isRoot) && !isRoot)
            {
                MIR_LOG_WARN("WorldSceneOps: cannot reparent a prefab instance member");
                return false;
            }
        }
        return true;
    }
}
