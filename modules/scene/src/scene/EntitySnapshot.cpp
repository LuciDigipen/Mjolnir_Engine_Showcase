/**
 * @file EntitySnapshot.cpp
 * @author Sumin Park
 * @brief Entity capture(+ children) as data.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "scene/EntitySnapshot.h"

#include "EntityCodec.h"
#include "core/ecs/TransformHierarchy.h"
#include "core/ecs/World.h"
#include "core/log/Log.h"

#include <serial/JsonFile.h>

#include <algorithm>
#include <iterator>
#include <map>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace mir
{
    namespace
    {
        using nlohmann::json;

        constexpr StableId kRootLocalId = 1;

        //keyed by snapshot local id, empty when the snapshot is empty or malformed
        std::map<StableId, Entity> SpawnSnapshot(World &world, const EntitySnapshot &snapshot, std::string_view caller)
        {
            if (snapshot.mData.empty())
                return {};

            std::optional<json> parsed = ParseJson(snapshot.mData, "EntitySnapshot");
            if (!parsed || !parsed->is_array())
            {
                MIR_LOG_ERROR("{}: snapshot is not an entity array, nothing restored", caller);
                return {};
            }

            std::vector<std::pair<StableId, json>> entities;
            entities.reserve(parsed->size());
            for (json &entity : *parsed)
            {
                if (!entity.is_object())
                    continue;

                const StableId id = entity.value("id", kNullStableId);
                entities.emplace_back(id, std::move(entity));
            }

            return SpawnEntities(world, entities, true);
        }
    }

    std::vector<Entity> SnapshotMembers(World &world, Entity root)
    {
        if (!world.IsAlive(root))
            return {};

        // parents precede children, so replaying SetParent in this order keeps sibling order
        std::vector<Entity> members{ root };
        ForEachDescendant(world, root, [&](Entity entity) { members.push_back(entity); });
        return members;
    }

    EntitySnapshot CaptureEntity(World &world, Entity root)
    {
        const std::vector<Entity> members = SnapshotMembers(world, root);
        if (members.empty())
            return {};

        EntityIdMap ids;
        ids.reserve(members.size());
        for (std::size_t i = 0; i < members.size(); ++i)
            ids[PackEntity(members[i])] = kRootLocalId + i;

        // undo and duplicate restore in the same run, so a reference leaving
        // the snapshot keeps its live handle
        json entities = json::array();
        for (std::size_t i = 0; i < members.size(); ++i)
            entities.push_back(EntityToJson(world, members[i], kRootLocalId + i, ids, true));

        return EntitySnapshot{ entities.dump() };
    }

    Entity RestoreEntity(World &world, const EntitySnapshot &snapshot, LoadedScene &scene, Entity parent)
    {
        const std::map<StableId, Entity> spawned = SpawnSnapshot(world, snapshot, "RestoreEntity");

        for (const auto &[localId, entity] : spawned)
            scene.mEntities[AllocateStableId(scene)] = entity;

        const auto root = spawned.find(kRootLocalId);
        if (root == spawned.end())
            return kNullEntity;

        if (!parent.IsNull())
            SetParent(world, root->second, parent, kNullEntity, false);
        return root->second;
    }

    std::vector<Entity> RestoreEntityAt(World &world, const EntitySnapshot &snapshot, LoadedScene &scene,
        std::span<const StableId> stableIds, Entity parent, Entity before)
    {
        const std::map<StableId, Entity> spawned = SpawnSnapshot(world, snapshot, "RestoreEntityAt");
        const auto root = spawned.find(kRootLocalId);
        if (root == spawned.end())
            return {};

        if (spawned.size() != stableIds.size())
            MIR_LOG_WARN("RestoreEntityAt: {} members but {} StableIds, the extra members stay unregistered",
                spawned.size(), stableIds.size());

        std::vector<Entity> members;
        members.reserve(spawned.size());
        for (const auto &[localId, entity] : spawned)
        {
            members.push_back(entity);

            const std::size_t index = static_cast<std::size_t>(localId - kRootLocalId);
            StableId id = index < stableIds.size() ? stableIds[index] : kNullStableId;
            if (id == kNullStableId)
                continue;

            if (scene.mEntities.contains(id))
            {
                MIR_LOG_WARN("RestoreEntityAt: StableId {} is already in use, the restored entity gets a new one", id);
                id = AllocateStableId(scene);
            }
            
            scene.mEntities[id] = entity;
            scene.mNextId = std::max(scene.mNextId, id + 1);
        }


        if (!before.IsNull() && SetParent(world, root->second, parent, before, false))
            return members;

        if (!parent.IsNull())
            SetParent(world, root->second, parent, kNullEntity, false);

        return members;
    }

    Entity DuplicateSceneEntity(World &world, LoadedScene &scene, Entity source)
    {
        if (!world.IsAlive(source))
            return kNullEntity;

        // read before the restore, which appends the copy to the same list
        Entity next = kNullEntity;
        const std::span<const Entity> siblings = world.HierarchyIndex().SiblingsOf(source);
        const auto it = std::find(siblings.begin(), siblings.end(), source);
        if (it != siblings.end() && std::next(it) != siblings.end())
            next = *std::next(it);

        const Entity parent = ParentOf(world, source);
        const Entity copy = RestoreEntity(world, CaptureEntity(world, source), scene, parent);

        // right after the source, as Unity's Ctrl+D
        if (!copy.IsNull() && !next.IsNull())
            SetParent(world, copy, parent, next, false);
        return copy;
    }
}
