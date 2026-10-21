/**
 * @file History.cpp
 * @author Dong Ta
 * @brief The editor's undo/redo steps, run once a frame at the end of DrawLayout.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <editor/history/History.h>

#include <editor/Selection.h>

#include <core/ecs/World.h>
#include <core/log/Log.h>

#include <algorithm>
#include <utility>

namespace mir
{
    void History::Submit(std::unique_ptr<ICommand> command)
    {
        if (command)
            mQueue.push_back(std::move(command));
    }

    void History::Flush(World &world, ISceneOps &scene)
    {
        CommandContext context{ world, mIds, scene, mAuthoring };

        std::vector<std::unique_ptr<ICommand>> queue = std::exchange(mQueue, {});
        for (std::unique_ptr<ICommand> &command : queue)
            Apply(std::move(command), context);

        
        if (std::exchange(mEndStep, false) || mPending != 0)
            CloseOpenStep();

        for (; mPending < 0 && CanUndo(); ++mPending)
        {
            if (!Undo(context))
                break;
        }

        for (; mPending > 0 && CanRedo(); --mPending)
        {
            if (!Redo(context))
                break;
        }
        mPending = 0;

        Trim();
        
        if (mIdsStale)
            DropUnusedIds();
    }

    void History::Clear()
    {
        std::vector<std::pair<DocumentId, bool>> moved;
        for (const auto &[document, state] : mDocuments)
            moved.emplace_back(document, MovedFromSave(document));

        mSteps.clear();
        mCursor = 0;
        mQueue.clear();
        mPending = 0;
        mEndStep = false;
        mIds.Clear();
        mIdsStale = false;
        mUsed = 0;

        //a fresh serial, so no old save point can match the empty history
        const uint64_t base = mNextSerial++;
        for (const auto &[document, wasMoved] : moved)
        {
            DocumentState &state = mDocuments[document];
            state.mBaseSerial = base;
            state.mSavedSerial = wasMoved ? std::nullopt : std::optional<uint64_t>(base);
        }
    }

    bool History::IsUnsaved(DocumentId document) const
    {
        const auto it = mDocuments.find(document);
        return MovedFromSave(document) || (it != mDocuments.end() && it->second.mMaintained);
    }

    void History::SetUnsaved(DocumentId document, bool unsaved)
    {
        DocumentState &state = mDocuments[document];
        if (unsaved)
        {
            state.mSavedSerial.reset();
            return;
        }

        CloseOpenStep();
        state.mSavedSerial = CurrentSerial(document);
        state.mMaintained = false;
    }

    uint64_t History::CurrentSerial(DocumentId document) const
    {
        for (std::size_t i = mCursor; i > 0; --i)
        {
            const Step &step = mSteps[i - 1];
            if (std::find(step.mDocuments.begin(), step.mDocuments.end(), document) != step.mDocuments.end())
                return step.mSerial;
        }

        const auto it = mDocuments.find(document);
        return it != mDocuments.end() ? it->second.mBaseSerial : 0;
    }

    bool History::MovedFromSave(DocumentId document) const
    {
        const auto it = mDocuments.find(document);
        const std::optional<uint64_t> saved =
            it != mDocuments.end() ? it->second.mSavedSerial : std::optional<uint64_t>(0);
        return !saved || *saved != CurrentSerial(document);
    }

    void History::MarkMaintained(const ICommand &command)
    {
        std::vector<DocumentId> maintained;
        command.CollectMaintained(maintained);
        for (const DocumentId document : maintained)
            mDocuments[document].mMaintained = true;
    }

    std::string History::UndoName() const
    {
        return CanUndo() ? mSteps[mCursor - 1].mCommand->Name() : std::string();
    }

    std::string History::RedoName() const
    {
        return CanRedo() ? mSteps[mCursor].mCommand->Name() : std::string();
    }

    void History::Apply(std::unique_ptr<ICommand> command, CommandContext &context)
    {
        std::vector<HistoryId> before = SelectionIds(context.mWorld);
        if (!command->Do(context))
        {
            mIdsStale = true;
            return;
        }
        MarkMaintained(*command);

        
        if (mCursor == mSteps.size() && !mSteps.empty() && mSteps.back().mOpen &&
            mSteps.back().mCommand->TryMerge(*command))
        {
            Step &open = mSteps.back();
            open.mSelectionAfter = SelectionIds(context.mWorld);
            Measure(open);
            return;
        }

        DropStepsFrom(mCursor);
        CloseOpenStep();

        Step step;
        step.mSerial = mNextSerial++;
        command->CollectDocuments(step.mDocuments);
        for (const DocumentId document : step.mDocuments)
            mDocuments.try_emplace(document);
        step.mCommand = std::move(command);
        step.mSelectionBefore = std::move(before);
        step.mSelectionAfter = SelectionIds(context.mWorld);
        step.mOpen = true;
        Measure(step);

        mSteps.push_back(std::move(step));
        mCursor = mSteps.size();
    }

    void History::CloseOpenStep()
    {
        if (!mSteps.empty())
            mSteps.back().mOpen = false;
    }

    //a refused undo leaves the cursor where it was
    bool History::Undo(CommandContext &context)
    {
        Step &step = mSteps[mCursor - 1];
        if (!step.mCommand->Undo(context))
        {
            MIR_LOG_WARN("History: could not undo '{}'", step.mCommand->Name());
            return false;
        }

        --mCursor;
        Measure(step);
        Select(context.mWorld, step.mSelectionBefore);
        return true;
    }

    bool History::Redo(CommandContext &context)
    {
        Step &step = mSteps[mCursor];
        if (!step.mCommand->Do(context))
        {
            MIR_LOG_WARN("History: could not redo '{}'", step.mCommand->Name());
            return false;
        }

        MarkMaintained(*step.mCommand);
        Measure(step);
        Select(context.mWorld, step.mSelectionAfter);
        ++mCursor;
        return true;
    }

    void History::DropStepsFrom(std::size_t index)
    {
        if (index >= mSteps.size())
            return;

        for (auto it = mSteps.begin() + index; it != mSteps.end(); ++it)
            mUsed -= it->mBytes;
        mSteps.erase(mSteps.begin() + index, mSteps.end());
        mIdsStale = true;
    }

   
    void History::Trim()
    {
        while (mUsed > mBudget && mSteps.size() > 1 && mCursor > 0)
        {
            const Step &dropped = mSteps.front();
            for (const DocumentId document : dropped.mDocuments)
            {
                DocumentState &state = mDocuments[document];
                if (state.mSavedSerial == state.mBaseSerial || state.mSavedSerial == dropped.mSerial)
                    state.mSavedSerial.reset();
                state.mBaseSerial = dropped.mSerial;
            }

            mUsed -= mSteps.front().mBytes;
            mSteps.pop_front();
            --mCursor;
            mIdsStale = true;
        }
    }

    
    void History::Measure(Step &step)
    {
        mUsed -= step.mBytes;
        step.mBytes = sizeof(Step) + step.mCommand->Size() +
            (step.mSelectionBefore.size() + step.mSelectionAfter.size()) * sizeof(HistoryId);
        mUsed += step.mBytes;
    }

    void History::DropUnusedIds()
    {
        std::vector<HistoryId> used;
        for (const Step &step : mSteps)
        {
            step.mCommand->CollectIds(used);
            used.insert(used.end(), step.mSelectionBefore.begin(), step.mSelectionBefore.end());
            used.insert(used.end(), step.mSelectionAfter.begin(), step.mSelectionAfter.end());
        }
        mIds.Retain(used);
        mIdsStale = false;
    }

    std::vector<HistoryId> History::SelectionIds(World &world)
    {
        std::vector<HistoryId> ids;
        if (const Selection *selection = world.TryResource<Selection>())
        {
            for (const Entity entity : selection->mSelected)
            {
                if (world.IsAlive(entity))
                    ids.push_back(mIds.IdOf(entity));
            }
        }
        return ids;
    }

    void History::Select(World &world, std::span<const HistoryId> ids) const
    {
        Selection *selection = world.TryResource<Selection>();
        if (selection == nullptr)
            return;

        selection->mSelected.clear();
        for (const HistoryId id : ids)
        {
            if (const Entity entity = mIds.EntityOf(id); world.IsAlive(entity))
                selection->mSelected.push_back(entity);
        }
    }
}
