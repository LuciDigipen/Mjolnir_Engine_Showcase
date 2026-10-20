/**
 * @file SceneAsset.h
 * @author Sumin Park
 * @brief The save-file identity for a scene and its entities.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once
#include "core/ecs/Entity.h"
#include "core/ecs/TransformHierarchy.h"
#include "core/ecs/World.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace mir
{

    // Save-file-only identity for an entity, distinct from Entity struct.
    // this is because Entity's slot and generation get reused across runs
    using StableId = uint64_t;

    // The "no entity" value for a StableId field
    inline constexpr StableId kNullStableId = 0;

    // Nested occurrence ids, then the final entity node id.
    using SceneNodeAddress = std::vector<uint64_t>;

    struct SceneFieldPatch
    {
        std::string mComponent;
        std::string mField;
        nlohmann::json mValue;
    };

    struct SceneComponentRecord
    {
        std::string mType;
        nlohmann::json mFields = nlohmann::json::object();
    };

    struct SceneInstancePatch
    {
        SceneNodeAddress mTarget;
        std::vector<SceneFieldPatch> mFields;
        std::vector<SceneComponentRecord> mAddedComponents;
        std::vector<std::string> mRemovedComponents;
    };

    struct ScenePrefabInstance
    {
        std::string mPrefab;
        StableId mRoot = kNullStableId;
        StableId mParent = kNullStableId;
        uint32_t mSiblingIndex = 0;
        nlohmann::json mPlacement = nlohmann::json::object();

        // every member including the root
        std::map<SceneNodeAddress, StableId> mBindings;

        std::vector<SceneInstancePatch> mPatches;
    };

    struct LoadedScene
    {
        std::string mName;

        // map, not unordered_map - the manifest is written in key order, so a
        // save with no change produces no diff
        std::map<StableId, Entity> mEntities;

        // keyed by root StableId
        std::map<StableId, ScenePrefabInstance> mInstances;

        // next id handed out, persisted in scene.json so a deleted entity's id
        // is never reissued
        StableId mNextId = 1;

        bool mDirty = false;
    };

    // Not backed by a file until the first save.
    inline LoadedScene NewScene(std::string name)
    {
        LoadedScene scene;
        scene.mName = std::move(name);
        return scene;
    }

    inline StableId AllocateStableId(LoadedScene &scene) { return scene.mNextId++; }

    // Create, allocate a StableId and register the entity
    inline Entity CreateSceneEntity(World &world, LoadedScene &scene, Entity parent = kNullEntity)
    {
        Entity entity = world.CreateEntity();
        scene.mEntities[AllocateStableId(scene)] = entity;
        if (!parent.IsNull())
            SetParent(world, entity, parent);
        return entity;
    }

    inline bool DestroySceneEntity(World &world, LoadedScene &scene, Entity root)
    {
        if (!world.IsAlive(root))
            return false;

        std::unordered_set<uint64_t> doomed{ PackEntity(root) };
        ForEachDescendant(world, root, [&](Entity entity) { doomed.insert(PackEntity(entity)); });

        bool erasedInstance = false;
        for (auto it = scene.mInstances.begin(); it != scene.mInstances.end();)
        {
            const auto entityIt = scene.mEntities.find(it->first);
            if (entityIt != scene.mEntities.end() && doomed.contains(PackEntity(entityIt->second)))
            {
                it = scene.mInstances.erase(it);
                erasedInstance = true;
            }
            else
            {
                ++it;
            }
        }
        if (erasedInstance)
            scene.mDirty = true;

        std::erase_if(scene.mEntities, [&](const auto &entry) { return doomed.contains(PackEntity(entry.second)); });

        // the hierarchy's destroy hook takes the descendants with it
        world.DestroyEntity(root);
        return true;
    }

    // Writes the member's World field and stores the override in its instance record.
    bool SetInstanceEntityRef(World &world, LoadedScene &scene, StableId root, const SceneNodeAddress &node,
        std::string_view component, std::string_view field, StableId target);
}
