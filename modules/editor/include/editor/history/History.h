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
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace mir
{
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

        void Clear();

        bool CanUndo() const { return mCursor > 0; }
        bool CanRedo() const { return mCursor < mSteps.size(); }
        std::string UndoName() const;
        std::string RedoName() const;

        std::size_t StepCount() const { return mSteps.size(); }
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
        };

        void Apply(std::unique_ptr<ICommand> command, CommandContext &context);
        void CloseOpenStep();
        void Undo(CommandContext &context);
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
    };
}
