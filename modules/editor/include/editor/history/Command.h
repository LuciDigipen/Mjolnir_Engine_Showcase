/**
 * @file Command.h
 * @author Dong Ta
 * @brief One undoable editor change, and what it receives when it runs.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <editor/history/EntityIds.h>

#include <cstddef>
#include <string>
#include <vector>

namespace mir
{
    class ISceneOps;
    class World;

    struct CommandContext
    {
        World &mWorld;
        EntityIds &mIds;
        ISceneOps &mScene;
    };

    class ICommand
    {
    public:
        virtual ~ICommand() = default;

        //also redo; false means nothing changed, so no step is recorded
        virtual bool Do(CommandContext &context) = 0;
        virtual void Undo(CommandContext &context) = 0;

        virtual std::size_t Size() const = 0;
        virtual std::string Name() const = 0;

        virtual void CollectIds(std::vector<HistoryId> &out) const = 0;
    };
}
