/**
 * @file DocumentNodeCommands.cpp
 * @author Jackey Cen
 * @brief Create, delete and reparent a document's nodes as one authored command each.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <editor/history/DocumentNodeCommands.h>

#include <editor/history/AuthoredView.h>
#include <editor/history/Authoring.h>
#include <editor/panels/HierarchyPanel.h>

#include <core/ecs/HierarchyIndex.h>
#include <core/ecs/World.h>
#include <core/log/Log.h>

#include <algorithm>
#include <cstddef>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace mir
{
    namespace
    {
        Entity DocumentRoot(const World &world)
        {
            const HierarchyIndex *index = world.TryResource<HierarchyIndex>();
            if (index == nullptr || index->Roots().empty())
                return kNullEntity;
            return index->Roots().front();
        }

        std::size_t SiblingIndex(const World &world, Entity parent, Entity before)
        {
            const HierarchyIndex *index = world.TryResource<HierarchyIndex>();
            if (index == nullptr)
                return 0;

            const std::span<const Entity> children = index->ChildrenOf(parent);
            if (before.IsNull())
                return children.size();

            const auto it = std::find(children.begin(), children.end(), before);
            return it == children.end() ? children.size() : static_cast<std::size_t>(std::distance(children.begin(), it));
        }

        std::unique_ptr<ICommand> FromAction(const AuthoredAction &action)
        {
            if (!action.mAllowed)
            {
                MIR_LOG_WARN("Editor: {} refused: {}", action.mName, action.mReason);
                return nullptr;
            }
            return MakeAuthoredCommand(action.mName, action.mEntries);
        }
    }

    std::unique_ptr<ICommand> MakeDocumentEntityCommand(const EntityAction &action, IAuthoredEntities &authored,
        World &world)
    {
        const Entity root = DocumentRoot(world);
        switch (action.mKind)
        {
        case EntityActionKind::Create:
        {
            if (root.IsNull())
                return nullptr;
            return FromAction(authored.CreateNode(root, SiblingIndex(world, root, kNullEntity), action.mPreset));
        }
        case EntityActionKind::Delete:
        {
            const Entity nodes[] = { action.mTarget };
            return FromAction(authored.DeleteNodes(nodes));
        }
        case EntityActionKind::Reparent:
        {
            if (root.IsNull() || action.mTargets.empty())
                return nullptr;

            const Entity parent = action.mParent.IsNull() ? root : action.mParent;
            const std::size_t index = SiblingIndex(world, parent, action.mBefore);
            std::vector<AuthoredEntry> entries;
            std::string name;
            for (const Entity target : action.mTargets)
            {
                const AuthoredAction moved = authored.ReparentNode(target, parent, index);
                if (!moved.mAllowed)
                    return FromAction(moved);
                if (name.empty())
                    name = moved.mName;
                entries.insert(entries.end(), moved.mEntries.begin(), moved.mEntries.end());
            }

            if (entries.empty())
                return nullptr;
            return MakeAuthoredCommand(name.empty() ? "Reparent Node" : name, std::move(entries));
        }
        case EntityActionKind::Duplicate:
        case EntityActionKind::CopyAsOrdinaryObject:
        case EntityActionKind::None:
            break;
        }
        return nullptr;
    }
}
