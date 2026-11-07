/**
 * @file WorldSceneOps.cpp
 * @author Dong Ta
 * @brief The scene calls the editor's history makes, done on a World and the scenes it holds.
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
#include <scene/SceneModule.h>

#include <optional>
#include <string>
#include <unordered_map>

namespace mir
{
    namespace
    {
        // An entity no scene owns is edited directly. Its records go into a throwaway scene with the session's ID,
        // so nothing is registered and new entities stay owned by the session.
        template <typename Fn>
        decltype(auto) InScene(World &world, LoadedScene *scene, Fn &&fn)
        {
            LoadedScene unowned;
            unowned.mInstanceId = kOwnedBySession;
            LoadedScene &target = scene != nullptr ? *scene : unowned;
            const World::SceneScope scope(world, target.mInstanceId);
            return fn(target);
        }
    }

    LoadedScene *WorldSceneOps::FindScene(SceneInstanceId instance)
    {
        SceneCollection *scenes = mWorld.TryResource<SceneCollection>();
        SceneEntry *entry = scenes != nullptr ? scenes->Find(instance) : nullptr;
        return entry != nullptr ? &entry->scene : nullptr;
    }

    Entity WorldSceneOps::CreateEntity()
    {
        return InScene(mWorld, FindScene(mWorld.PrimaryScene()),
            [&](LoadedScene &scene) { return CreateSceneEntity(mWorld, scene); });
    }

    Entity WorldSceneOps::Duplicate(Entity source)
    {
        return InScene(mWorld, FindScene(mWorld.EntityScene(source)),
            [&](LoadedScene &scene) { return DuplicateSceneEntitySubtree(mWorld, scene, source); });
    }

    Entity WorldSceneOps::CopyAsOrdinaryObject(Entity source)
    {
        return InScene(mWorld, FindScene(mWorld.EntityScene(source)),
            [&](LoadedScene &scene) { return CopyInstanceMemberAsOrdinary(mWorld, scene, source); });
    }

    CapturedSubtree WorldSceneOps::Capture(Entity root)
    {
        World &world = mWorld;

        CapturedSubtree subtree;
        subtree.mMembers = SnapshotMembers(world, root);
        subtree.mData = CaptureEntity(world, root).mData;

        const LoadedScene *scene = FindScene(world.EntityScene(root));
        subtree.mScene = scene != nullptr ? scene->mInstanceId : kOwnedBySession;

        std::unordered_map<uint64_t, StableId> ids;
        if (scene != nullptr)
            for (const auto &[id, entity] : scene->mEntities)
                ids[PackEntity(entity)] = id;

        subtree.mStableIds.reserve(subtree.mMembers.size());
        for (const Entity member : subtree.mMembers)
        {
            const auto it = ids.find(PackEntity(member));
            subtree.mStableIds.push_back(it != ids.end() ? it->second : kNullStableId);
        }

        if (scene != nullptr)
            subtree.mInstances = CaptureInstanceRecords(*scene, subtree.mStableIds);

        return subtree;
    }

    std::vector<Entity> WorldSceneOps::Restore(const CapturedSubtree &subtree, Entity parent, Entity before)
    {
        LoadedScene *owner = FindScene(subtree.mScene);
        if (owner == nullptr && subtree.mScene != kOwnedBySession)
            MIR_LOG_WARN("WorldSceneOps: the entity's scene is no longer loaded, so it is restored without one");

        return InScene(mWorld, owner, [&](LoadedScene &scene)
            {
                std::vector<Entity> members = RestoreEntityAt(mWorld, EntitySnapshot{ subtree.mData }, scene,
                    subtree.mStableIds, parent, before);
                if (owner != nullptr && !members.empty() && !subtree.mInstances.empty() &&
                    !RestoreInstanceRecords(scene, subtree.mInstances))
                    MIR_LOG_WARN("WorldSceneOps: could not restore a prefab instance record");
                return members;
            });
    }

    bool WorldSceneOps::Destroy(Entity root)
    {
        LoadedScene *owner = FindScene(mWorld.EntityScene(root));
        if (owner != nullptr)
        {
            StableId instanceRoot = kNullStableId;
            const std::optional<StableId> rootId = StableIdOf(*owner, root);
            if (rootId && FindInstanceBinding(*owner, *rootId, instanceRoot) != nullptr && instanceRoot != *rootId)
            {
                MIR_LOG_WARN("WorldSceneOps: cannot delete a prefab instance member; delete its root instead");
                return false;
            }
        }

        return InScene(mWorld, owner, [&](LoadedScene &scene) { return DestroySceneEntity(mWorld, scene, root); });
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
            const LoadedScene *parentScene = FindScene(mWorld.EntityScene(parent));
            const std::optional<StableId> parentId =
                parentScene != nullptr ? StableIdOf(*parentScene, parent) : std::nullopt;
            if (parentId && FindInstanceBinding(*parentScene, *parentId, instanceRoot) != nullptr)
            {
                MIR_LOG_WARN("WorldSceneOps: cannot reparent under a prefab instance member");
                return false;
            }
        }

        for (const Entity entity : entities)
        {
            // Each scene saves and unloads its own entities, so a parent from another scene is refused.
            if (!parent.IsNull() && mWorld.EntityScene(entity) != mWorld.EntityScene(parent))
            {
                MIR_LOG_WARN("WorldSceneOps: cannot reparent under an entity from another scene");
                return false;
            }

            const LoadedScene *scene = FindScene(mWorld.EntityScene(entity));
            const std::optional<StableId> id = scene != nullptr ? StableIdOf(*scene, entity) : std::nullopt;
            if (id && FindInstanceBinding(*scene, *id, instanceRoot) != nullptr && instanceRoot != *id)
            {
                MIR_LOG_WARN("WorldSceneOps: cannot reparent a prefab instance member");
                return false;
            }
        }
        return true;
    }
}
