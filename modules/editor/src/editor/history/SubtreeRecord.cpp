/**
 * @file SubtreeRecord.cpp
 * @author Dong Ta
 * @brief An entity and its descendants taken out of the scene, and what it takes to put them back.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "SubtreeRecord.h"

#include <core/ecs/ComponentRegistry.h>
#include <core/ecs/TransformHierarchy.h>
#include <core/ecs/World.h>
#include <core/log/Log.h>

#include <algorithm>
#include <iterator>
#include <span>
#include <unordered_set>

namespace mir
{
    Entity NextSibling(const World &world, Entity entity)
    {
        const std::span<const Entity> siblings = world.HierarchyIndex().SiblingsOf(entity);
        const auto it = std::find(siblings.begin(), siblings.end(), entity);
        if (it == siblings.end() || std::next(it) == siblings.end())
            return kNullEntity;
        return *std::next(it);
    }

    Entity LiveEntity(const World &world, const EntityIds &ids, HistoryId id)
    {
        const Entity entity = ids.EntityOf(id);
        return world.IsAlive(entity) ? entity : kNullEntity;
    }

    bool SubtreeRecord::Remove(CommandContext &context, Entity root)
    {
        World &world = context.mWorld;
        if (!world.IsAlive(root))
            return false;

        mSubtree = context.mScene.Capture(root);
        if (mSubtree.mMembers.empty())
            return false;

        mMembers.clear();
        for (const Entity member : mSubtree.mMembers)
            mMembers.push_back(context.mIds.IdOf(member));

        mParent = context.mIds.IdOf(ParentOf(world, root));
        mNext = context.mIds.IdOf(NextSibling(world, root));
        FindRefLinks(context);

        if (!context.mScene.Destroy(root))
            return false;

        for (const HistoryId id : mMembers)
            context.mIds.Unbind(id);
        return true;
    }

    Entity SubtreeRecord::Restore(CommandContext &context)
    {
        World &world = context.mWorld;
        const std::vector<Entity> restored = context.mScene.Restore(mSubtree,
            LiveEntity(world, context.mIds, mParent), LiveEntity(world, context.mIds, mNext));
        if (restored.empty())
            return kNullEntity;

        if (restored.size() != mMembers.size())
            MIR_LOG_WARN("SubtreeRecord: {} members captured, {} restored", mMembers.size(), restored.size());

        for (std::size_t i = 0; i < std::min(restored.size(), mMembers.size()); ++i)
            context.mIds.Rebind(mMembers[i], restored[i]);

        for (const RefLink &link : mLinks)
        {
            const Entity referrer = LiveEntity(world, context.mIds, link.mReferrer);
            if (referrer.IsNull() || !link.mOps->Has(world, referrer))
                continue;

            const Entity target = LiveEntity(world, context.mIds, link.mTarget);
            link.mOps->mFields[link.mField].Write(link.mOps->GetComponent(world, referrer), &target);
        }
        return restored.front();
    }

    std::size_t SubtreeRecord::Size() const
    {
        std::size_t instances = 0;
        for (const std::string &record : mSubtree.mInstances)
            instances += record.size();

        return mSubtree.mData.size() + instances + mSubtree.mMembers.size() * sizeof(Entity) +
            mSubtree.mStableIds.size() * sizeof(uint64_t) + mMembers.size() * sizeof(HistoryId) +
            mLinks.size() * sizeof(RefLink);
    }

    void SubtreeRecord::CollectIds(std::vector<HistoryId> &out) const
    {
        out.insert(out.end(), mMembers.begin(), mMembers.end());
        out.push_back(mParent);
        out.push_back(mNext);
        for (const RefLink &link : mLinks)
        {
            out.push_back(link.mReferrer);
            out.push_back(link.mTarget);
        }
    }

    
    void SubtreeRecord::FindRefLinks(CommandContext &context)
    {
        World &world = context.mWorld;
        mLinks.clear();

        struct RefField
        {
            const ComponentOps *mOps;
            uint32_t mField;
        };
        std::vector<RefField> refFields;
        for (const ComponentOps *ops : ComponentRegistry::Instance().All())
        {
            for (uint32_t field = 0; field < ops->mFields.size(); ++field)
            {
                if (ops->mFields[field].mKind == FieldKind::EntityRef)
                    refFields.push_back({ ops, field });
            }
        }
        if (refFields.empty())
            return;

        std::unordered_set<uint64_t> inside;
        for (const Entity member : mSubtree.mMembers)
            inside.insert(PackEntity(member));

        world.ForEachAliveEntity([&](Entity referrer)
            {
                const bool referrerInside = inside.contains(PackEntity(referrer));
                for (const RefField &ref : refFields)
                {
                    if (!ref.mOps->Has(world, referrer))
                        continue;

                    Entity target = kNullEntity;
                    ref.mOps->mFields[ref.mField].Read(ref.mOps->GetComponent(world, referrer), &target);
                    if (!world.IsAlive(target) || inside.contains(PackEntity(target)) == referrerInside)
                        continue;

                    mLinks.push_back({ context.mIds.IdOf(referrer), ref.mOps, ref.mField, context.mIds.IdOf(target) });
                }
            });
    }
}
