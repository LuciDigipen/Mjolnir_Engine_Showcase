/**
 * @file EntityCodec.h
 * @author Sumin Park
 * @brief Entity <-> Json converter
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once
#include "core/ecs/Entity.h"
#include "scene/SceneAsset.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mir
{
    class World;
    struct ComponentOps;

    using EntityIdMap = std::unordered_map<uint64_t, StableId>;

    struct PendingEntityRef
    {
        Entity mEntity = kNullEntity;
        const ComponentOps *mOps = nullptr;
        std::string mField;
        nlohmann::json mTarget;
    };

    nlohmann::json EntityToJson(World &world, Entity entity, StableId id, const EntityIdMap &ids,
        bool keepOutsideRefs);

    // Position among the siblings in `ids`, so entities outside it leave no gaps
    uint32_t SiblingIndexOf(const World &world, Entity entity, const EntityIdMap &ids);

    uint32_t EntitySiblingIndexFromJson(const nlohmann::json &entityJson);

    std::optional<StableId> ReadStableIdJson(const nlohmann::json &value);

    // Adds the entity's components and non-EntityRef fields; EntityRef fields
    // are appended to pendingRefs for a later pass.
    void ApplyEntityComponents(World &world, Entity entity, const nlohmann::json &entityJson, StableId id,
        std::vector<PendingEntityRef> &pendingRefs);

    void ResolvePendingEntityRefs(World &world, const std::vector<PendingEntityRef> &pendingRefs,
        const std::map<StableId, Entity> &spawned, bool resolveOutsideRefs);

    std::map<StableId, Entity> SpawnEntities(World &world,
        const std::vector<std::pair<StableId, nlohmann::json>> &entities, bool resolveOutsideRefs);
}
