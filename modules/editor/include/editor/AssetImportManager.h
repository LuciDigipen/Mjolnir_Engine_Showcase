/**
 * @file AssetImportManager.h
 * @author Jackey Cen
 * @brief Editor-side import queue: stages dropped files, runs AssetCooker batches and applies results in Edit.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <project/GameProject.h>
#include <core/platform/Process.h>
#include <editor/panels/AssetImportPanel.h>

#include <chrono>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>

namespace mir
{
    // Editor-side owner for finite AssetCooker batches. It never keeps a
    // cooker process alive while the queue is empty.
    class AssetImportManager
    {
    public:
        // Throws SetupError when the import workflow cannot run
        void Initialize(const GameProject &project);

        // Safe without Initialize; stops a running AssetCooker
        void Shutdown();

        // Cooks every source asset in the next batch, for a build that has no manifest yet
        void QueueFullCook();
        bool SourceHasAssets() const;
        bool QueueChanged(const std::filesystem::path &path);
        const std::filesystem::path &SourceAssetRoot() const { return mSourceAssetRoot; }
        void Handle(const AssetImportAction &action);
        void Update(bool editing);

        const AssetImportView &View() const { return mView; }
        bool PendingApply() const { return mPendingApply && !mView.mProcessRunning && mQueued.Empty(); }
        void MarkApplied();
        void MarkApplyFailed(std::string diagnostic);
        void Reject(const std::string &model, std::string diagnostic);

        // Queues one authored .material.json for a finite cook batch and returns
        // the request identity the apply ack must match. force re-cooks and
        // re-applies unchanged content, for a retry after renderer publication failed.
        uint64_t ReimportMaterial(std::string_view logicalPath, bool force = false);
        // The last request included in a committed cooked catalog.
        std::optional<uint64_t> MaterialCookRevision(std::string_view logicalPath) const;
        std::optional<uint64_t> MaterialCookFailureRevision(std::string_view logicalPath) const;
        uint64_t ApplySequence() const { return mApplySequence; }

    private:
        struct Batch
        {
            std::set<std::string> models, materials;
            std::set<std::filesystem::path> changed;
            std::set<std::string> removals;
            std::map<std::string, std::filesystem::path> moves;
            std::map<std::string, uint64_t> materialSequences;
            bool force = false;
            // no --model arguments: AssetCooker cooks everything and writes a fresh manifest
            bool fullCook = false;
            std::filesystem::path importSource, packageRoot, targetDirectory;
            bool Empty() const
            {
                return models.empty() && materials.empty() && changed.empty() && removals.empty() && moves.empty() &&
                       importSource.empty() && !fullCook;
            }
            void Clear() { *this = {}; }
        };
        struct WatchedFile
        {
            uint64_t signature = 0;
            std::chrono::steady_clock::time_point changedAt{};
            bool pending = false;
        };

        void StartBatch();
        void FinishBatch(int exitCode, bool editing);
        void QueueExternalImport(const AssetImportAction &action);
        std::filesystem::path UniqueImportTarget(const std::filesystem::path &source) const;
        void RefreshView();
        void PollWatcher();
        void SetStatus(const std::string &model, AssetImportStatus status, std::string diagnostic = {});
        void SaveAutoImport();

        GameProject mProject;
        std::filesystem::path mSourceRoot, mSourceAssetRoot, mSourceSettingsPath, mCooker, mCookedRoot, mResultPath;
        ChildProcess mProcess;
        Batch mQueued, mRunning;
        AssetImportView mView;
        std::unordered_map<std::string, AssetImportStatus> mStatus;
        std::unordered_map<std::string, std::string> mDiagnostics;
        uint64_t mMaterialRequestSequence = 0;
        std::unordered_map<std::string, uint64_t> mMaterialCookRevision;
        std::unordered_map<std::string, uint64_t> mMaterialFailureRevision;
        std::unordered_map<std::string, uint64_t> mPendingApplyRevision;
        uint64_t mApplySequence = 0;
        std::map<std::filesystem::path, WatchedFile> mWatched;
        std::chrono::steady_clock::time_point mLastWatchPoll{};
        std::optional<uint64_t> mImportCacheSignature;
        bool mViewDirty = true;
        bool mPendingApply = false;
        bool mCancelRequested = false;
        bool mWatchInitialized = false;
        bool mRunningStale = false;
    };
} // namespace mir
