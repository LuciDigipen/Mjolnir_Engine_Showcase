/**
 * @file AppSceneOps.cpp
 * @author Dong Ta
 * @brief The scene calls the editor's history makes, done on App's World and LoadedScene.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "AppSceneOps.h"

#include "PrefabWorkspace.h"
#include "SceneResolve.h"

#include <app/App.h>
#include <core/log/Log.h>
#include <prefab/PrefabId.h>
#include <scene/EntitySnapshot.h>
#include <scene/SceneAsset.h>
#include <scene/SceneDuplicate.h>
#include <scene/SceneIO.h>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace mir
{
    Entity AppSceneOps::CreateEntity()
    {
        return CreateSceneEntity(mApp.GetWorld(), mApp.Scene());
    }

    Entity AppSceneOps::Duplicate(Entity source)
    {
        const Entity copy = DuplicateSceneEntitySubtree(mApp.GetWorld(), mApp.Scene(), source);
        ResolveScripts();
        return copy;
    }

    Entity AppSceneOps::CopyAsOrdinaryObject(Entity source)
    {
        const Entity copy = CopyInstanceMemberAsOrdinary(mApp.GetWorld(), mApp.Scene(), source);
        ResolveScripts();
        return copy;
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

        subtree.mInstances = CaptureInstanceRecords(mApp.Scene(), subtree.mStableIds);

        return subtree;
    }

    std::vector<Entity> AppSceneOps::Restore(const CapturedSubtree &subtree, Entity parent, Entity before)
    {
        std::vector<Entity> members = RestoreEntityAt(mApp.GetWorld(), EntitySnapshot{ subtree.mData }, mApp.Scene(),
            subtree.mStableIds, parent, before);
        if (!members.empty() && !subtree.mInstances.empty() &&
            !RestoreInstanceRecords(mApp.Scene(), subtree.mInstances))
        {
            MIR_LOG_WARN("AppSceneOps: could not restore a prefab instance record");
        }
        ResolveScripts();
        return members;
    }

    bool AppSceneOps::Destroy(Entity root)
    {
        LoadedScene &scene = mApp.Scene();
        StableId rootId = kNullStableId;
        for (const auto &[id, entity] : scene.mEntities)
        {
            if (entity == root)
            {
                rootId = id;
                break;
            }
        }

        if (rootId != kNullStableId)
        {
            for (const auto &[instanceRoot, instance] : scene.mInstances)
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

                MIR_LOG_WARN("AppSceneOps: cannot delete a prefab instance member; delete its root instead");
                return false;
            }
        }

        return DestroySceneEntity(mApp.GetWorld(), scene, root);
    }

    Entity AppSceneOps::PlacePrefab(std::string_view contentPath, Entity parent, const WorldVec3 &position)
    {
        PrefabWorkspace *documents = mApp.OpenPrefabs();
        if (documents == nullptr)
        {
            MIR_LOG_ERROR("Place Prefab: Prefab documents are unavailable");
            return kNullEntity;
        }

        const std::optional<PrefabId> id =
            documents->FindByPath(mApp.Project().mRootDir / std::filesystem::path(std::string(contentPath)));
        if (!id)
        {
            MIR_LOG_ERROR("Place Prefab: '{}' is not a registered Prefab", contentPath);
            return kNullEntity;
        }

        const Entity root = documents->Place(mApp.GetWorld(), *id, parent, position);
        if (!root.IsNull())
            ResolveScripts();
        return root;
    }

    bool AppSceneOps::CanReparent(std::span<const Entity> entities, Entity parent)
    {
        const LoadedScene &scene = mApp.Scene();
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
                MIR_LOG_WARN("AppSceneOps: cannot reparent under a prefab instance member");
                return false;
            }
        }

        for (const Entity entity : entities)
        {
            bool isRoot = false;
            const StableId id = stableIdOf(entity);
            if (id != kNullStableId && memberState(id, isRoot) && !isRoot)
            {
                MIR_LOG_WARN("AppSceneOps: cannot reparent a prefab instance member");
                return false;
            }
        }
        return true;
    }

    void AppSceneOps::ResolveScripts()
    {
        ResolveSceneScripts(mApp.GetWorld(), mApp.Scripts(), mApp.ScriptReload(), mApp.Assets(),
            mApp.Project().mAssetIdRoot);
    }
}
