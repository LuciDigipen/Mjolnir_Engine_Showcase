/**
 * @file EntityIds.cpp
 * @author Dong Ta
 * @brief History's own id per entity, kept across a delete and its undo.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <editor/history/EntityIds.h>

#include <unordered_set>

namespace mir
{
    HistoryId EntityIds::IdOf(Entity entity)
    {
        if (entity.IsNull())
            return kNullHistoryId;

        const auto [it, added] = mIds.try_emplace(PackEntity(entity), mNextId);

        if (added)
            mEntities[mNextId++] = entity;

        return it->second;
    }

    Entity EntityIds::EntityOf(HistoryId id) const
    {
        const auto it = mEntities.find(id);

        return it != mEntities.end() ? it->second : kNullEntity;
    }

    void EntityIds::Rebind(HistoryId id, Entity entity)
    {
        if (id == kNullHistoryId)
            return;

        Unbind(id);
        mEntities[id] = entity;
        if (entity.IsNull())
            return;

        
        const auto [it, added] = mIds.try_emplace(PackEntity(entity), id);
        
        if (!added && it->second != id)
        {
            mEntities[it->second] = kNullEntity;
            it->second = id;
        }
    }

    void EntityIds::Unbind(HistoryId id)
    {
        const auto it = mEntities.find(id);

        if (it == mEntities.end() || it->second.IsNull())
            return;

        mIds.erase(PackEntity(it->second));
        it->second = kNullEntity;
    }

    void EntityIds::Retain(std::span<const HistoryId> used)
    {
        const std::unordered_set<HistoryId> keep(used.begin(), used.end());
        std::erase_if(mEntities, [&](const auto &entry) { return !keep.contains(entry.first); });
        std::erase_if(mIds, [&](const auto &entry) { return !keep.contains(entry.second); });
    }

    void EntityIds::Clear()
    {
        mEntities.clear();
        mIds.clear();
    }
}
