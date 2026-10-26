/**
 * @file History.h
 * @author Dong Ta
 * @brief The editor's undo/redo steps, run once a frame at the end of DrawLayout.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <editor/history/Command.h>
#include <editor/history/EntityIds.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace mir
{
    class IAuthoringBackend;
    class ISceneOps;
    class World;

    inline constexpr std::size_t kHistoryBudget = 64ull * 1024 * 1024;

    class History
    {
    public:
        explicit History(std::size_t budget = kHistoryBudget) : mBudget(budget) {}

        void Submit(std::unique_ptr<ICommand> command);
        void RequestUndo() { --mPending; }
        void RequestRedo() { ++mPending; }

       
        void EndStep() { mEndStep = true; }

        //runs this frame's commands, then the requested undos and redos
        void Flush(World &world, ISceneOps &scene);

        
        void SetAuthoringBackend(IAuthoringBackend *backend) { mAuthoring = backend; }

        
        void Clear();

        //true when the document differs from its last save, or kept a change Undo does not reverse
        bool IsUnsaved(DocumentId document = kSceneDocument) const;

        //false = the document counts as saved from now on (call it after a save).
        //true = unsaved until the next save
        void SetUnsaved(bool unsaved) { SetUnsaved(kSceneDocument, unsaved); }
        void SetUnsaved(DocumentId document, bool unsaved);

        bool CanUndo() const { return mCursor > 0; }
        bool CanRedo() const { return mCursor < mSteps.size(); }
        std::string UndoName() const;
        std::string RedoName() const;

        std::size_t StepCount() const { return mSteps.size(); }
        // Commands accepted since construction; a pending session compares it across one frame.
        std::size_t SubmissionCount() const { return mSubmissions; }
        std::size_t Cursor() const { return mCursor; }
        std::size_t UsedBytes() const { return mUsed; }
        const EntityIds &Ids() const { return mIds; }

    private:
        struct Step
        {
            uint64_t mSerial = 0;
            std::unique_ptr<ICommand> mCommand;
            std::vector<HistoryId> mSelectionBefore;
            std::vector<HistoryId> mSelectionAfter;
            std::size_t mBytes = 0;
            bool mOpen = false; // still taking merges
            std::vector<DocumentId> mDocuments;
        };

        struct DocumentState
        {
            uint64_t mBaseSerial = 0;                 
            std::optional<uint64_t> mSavedSerial = 0; // empty = the saved state is out of reach
            bool mMaintained = false;                 
        };

        //the newest applied step that changed the document, or the document's bottom of history
        uint64_t CurrentSerial(DocumentId document) const;
        bool MovedFromSave(DocumentId document) const;
        void MarkMaintained(const ICommand &command);

        void Apply(std::unique_ptr<ICommand> command, CommandContext &context);
        void CloseOpenStep();
        bool Undo(CommandContext &context);
        bool Redo(CommandContext &context);

        void DropStepsFrom(std::size_t index);
        void Trim();
        void Measure(Step &step);
        void DropUnusedIds();

        std::vector<HistoryId> SelectionIds(World &world);
        void Select(World &world, std::span<const HistoryId> ids) const;

        std::deque<Step> mSteps;
        std::size_t mCursor = 0;
        std::vector<std::unique_ptr<ICommand>> mQueue;
        int mPending = 0; // below 0 undoes, above 0 redoes
        bool mEndStep = false;

        EntityIds mIds;
        bool mIdsStale = false;

        std::size_t mUsed = 0;
        std::size_t mBudget;
        uint64_t mNextSerial = 1;
        std::size_t mSubmissions = 0;

        //serials only go up, so a serial dropped from history never matches again
        std::map<DocumentId, DocumentState> mDocuments;

        IAuthoringBackend *mAuthoring = nullptr;
    };
}
