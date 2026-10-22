/**
 * @file SubtreeRecord.h
 * @author Dong Ta
 * @brief An entity and its descendants taken out of the scene, and what it takes to put them back.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <editor/history/Command.h>
#include <editor/history/SceneOps.h>

#include <core/ecs/Entity.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mir
{
    struct ComponentOps;

    class SubtreeRecord
    {
    public:
        
        bool Remove(CommandContext &context, Entity root);

        //the root, or null when nothing came back
        Entity Restore(CommandContext &context);

        std::size_t Size() const;
        void CollectIds(std::vector<HistoryId> &out) const;

    private:
        
        struct RefLink
        {
            HistoryId mReferrer = kNullHistoryId;
            const ComponentOps *mOps = nullptr;
            uint32_t mField = 0;
            HistoryId mTarget = kNullHistoryId;
        };

        void FindRefLinks(CommandContext &context);

        CapturedSubtree mSubtree;
        std::vector<HistoryId> mMembers;
        HistoryId mParent = kNullHistoryId;
        HistoryId mNext = kNullHistoryId;
        std::vector<RefLink> mLinks;
    };

    Entity NextSibling(const World &world, Entity entity);

    //null when the entity is gone
    Entity LiveEntity(const World &world, const EntityIds &ids, HistoryId id);
}
