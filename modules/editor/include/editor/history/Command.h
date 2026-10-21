/**
 * @file Command.h
 * @author Dong Ta
 * @brief One undoable editor change, and what it receives when it runs.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <editor/history/DocumentId.h>
#include <editor/history/EntityIds.h>

#include <cstddef>
#include <string>
#include <vector>

namespace mir
{
    class IAuthoringBackend;
    class ISceneOps;
    class World;

    struct CommandContext
    {
        World &mWorld;
        EntityIds &mIds;
        ISceneOps &mScene;
        IAuthoringBackend *mAuthoring = nullptr; // null until App supplies one
    };

    class ICommand
    {
    public:
        virtual ~ICommand() = default;

        //also redo; false means nothing changed, so no step is recorded
        virtual bool Do(CommandContext &context) = 0;

        //false means nothing changed, so the cursor stays
        virtual bool Undo(CommandContext &context) = 0;

        virtual std::size_t Size() const = 0;
        virtual std::string Name() const = 0;


        virtual bool TryMerge(const ICommand &) { return false; }

        virtual void CollectIds(std::vector<HistoryId> &out) const = 0;

        //the documents whose save state this step moves; scene commands change the scene
        virtual void CollectDocuments(std::vector<DocumentId> &out) const { out.push_back(kSceneDocument); }

        //documents the last Do changed in a way Undo leaves alone, so they stay unsaved until saved
        virtual void CollectMaintained(std::vector<DocumentId> &) const {}
    };
}
