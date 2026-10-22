/**
 * @file EntityIds.h
 * @author Dong Ta
 * @brief History's own id per entity, kept across a delete and its undo.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/ecs/Entity.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>

namespace mir
{
    using HistoryId = uint64_t;
    inline constexpr HistoryId kNullHistoryId = 0;

    class EntityIds
    {
    public:
        //a new id the first time an entity is seen
        HistoryId IdOf(Entity entity);


        Entity EntityOf(HistoryId id) const;

        void Rebind(HistoryId id, Entity entity);
        void Unbind(HistoryId id);

        //drops every id not in used
        void Retain(std::span<const HistoryId> used);

        void Clear();
        std::size_t Size() const { return mEntities.size(); }

    private:
        std::unordered_map<HistoryId, Entity> mEntities;
        std::unordered_map<uint64_t, HistoryId> mIds; // by PackEntity
        HistoryId mNextId = 1;
    };
}
