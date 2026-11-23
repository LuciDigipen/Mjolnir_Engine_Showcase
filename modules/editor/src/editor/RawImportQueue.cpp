/**
 * @file RawImportQueue.cpp
 * @author Jackey Cen
 * @brief Per-path state of the raw source files an import batch cooks.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <editor/RawImportQueue.h>

#include <utility>

namespace mir
{
    std::string RawImportQueue::Key(const std::filesystem::path &path) { return path.generic_string(); }

    void RawImportQueue::Queue(const std::filesystem::path &path)
    {
        mVetoed.erase(Key(path));
        mStatuses[Key(path)] = RawImportStatus{RawImportState::Queued, {}};
    }

    void RawImportQueue::StartCooking(const std::filesystem::path &path)
    {
        const auto found = mStatuses.find(Key(path));
        if (found != mStatuses.end() && found->second.mState == RawImportState::Queued)
            found->second = RawImportStatus{RawImportState::Cooking, {}};
    }

    void RawImportQueue::Finish(const std::filesystem::path &path, bool applyPending, bool editing)
    {
        const auto found = mStatuses.find(Key(path));
        if (found == mStatuses.end() || found->second.mState != RawImportState::Cooking)
            return;
        if (!applyPending)
            found->second = RawImportStatus{RawImportState::Applied, {}};
        else
            found->second = RawImportStatus{
                editing ? RawImportState::WaitingForApply : RawImportState::WaitingForStop, {}};
    }

    void RawImportQueue::Skip(const std::filesystem::path &path, std::string reason)
    {
        const auto found = mStatuses.find(Key(path));
        if (found != mStatuses.end() && found->second.mState == RawImportState::Cooking)
            found->second = RawImportStatus{RawImportState::Skipped, std::move(reason)};
    }

    void RawImportQueue::Fail(const std::filesystem::path &path, std::string reason)
    {
        mVetoed.erase(Key(path));
        mStatuses[Key(path)] = RawImportStatus{RawImportState::Failed, std::move(reason)};
    }

    void RawImportQueue::MarkApplied()
    {
        mUnapplied = false;
        for (auto &[key, status] : mStatuses)
            if (status.mState == RawImportState::WaitingForApply || status.mState == RawImportState::WaitingForStop ||
                mVetoed.contains(key))
                status = RawImportStatus{RawImportState::Applied, {}};
        mVetoed.clear();
    }

    void RawImportQueue::MarkApplyFailed(const std::string &reason)
    {
        mUnapplied = true;
        for (auto &[key, status] : mStatuses)
            if (status.mState == RawImportState::WaitingForApply || status.mState == RawImportState::WaitingForStop)
            {
                status = RawImportStatus{RawImportState::Failed, reason};
                mVetoed.insert(key);
            }
    }

    bool RawImportQueue::NeedsApply(bool cookedOutput, bool listedFiles) const
    {
        return cookedOutput || (mUnapplied && listedFiles);
    }

    std::optional<RawImportStatus> RawImportQueue::Status(const std::filesystem::path &path) const
    {
        const auto found = mStatuses.find(Key(path));
        return found != mStatuses.end() ? std::optional<RawImportStatus>(found->second) : std::nullopt;
    }
} // namespace mir
