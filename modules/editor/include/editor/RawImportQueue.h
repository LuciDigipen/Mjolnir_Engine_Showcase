/**
 * @file RawImportQueue.h
 * @author Jackey Cen
 * @brief Per-path state of the raw source files an import batch cooks.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>

namespace mir
{
    enum class RawImportState
    {
        Queued,
        Cooking,
        WaitingForStop,
        WaitingForApply,
        Applied,
        // the cook left the file out, so the runtime does not get it
        Skipped,
        Failed,
    };

    struct RawImportStatus
    {
        RawImportState mState = RawImportState::Queued;
        std::string mReason;
    };

    class RawImportQueue
    {
    public:
        void Queue(const std::filesystem::path &path);
        void StartCooking(const std::filesystem::path &path);
        // A batch only moves the paths it started, so one queued again while cooking stays queued.
        void Finish(const std::filesystem::path &path, bool applyPending, bool editing);
        void Skip(const std::filesystem::path &path, std::string reason);
        void Fail(const std::filesystem::path &path, std::string reason);
        // Also applies files a vetoed apply left cooked, unless they were queued or failed since.
        void MarkApplied();
        void MarkApplyFailed(const std::string &reason);
        // A vetoed apply leaves the cooked manifest unapplied until a later changed batch requests Apply again.
        bool NeedsApply(bool cookedOutput, bool listedFiles) const;
        std::optional<RawImportStatus> Status(const std::filesystem::path &path) const;

    private:
        static std::string Key(const std::filesystem::path &path);
        std::map<std::string, RawImportStatus> mStatuses;
        std::set<std::string> mVetoed;
        bool mUnapplied = false;
    };
} // namespace mir
