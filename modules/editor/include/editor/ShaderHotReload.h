/**
 * @file ShaderHotReload.h
 * @author Jackey Cen
 * @brief Development-only shader compilation and transactional publication.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 */
#pragma once

#include <renderer/ShaderCompiler.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <map>
#include <optional>
#include <set>

namespace mir
{
    class VulkanRenderer;
    class JobExecutor;
    class BackgroundJobQueue;

    struct ShaderReloadBackend
    {
        std::function<bool()> available;
        std::function<bool(const std::filesystem::path &, std::string *)> prepare;
        std::function<void()> commit;
        std::function<void()> discard;
    };

    class ShaderHotReload
    {
    public:
        ShaderHotReload();
        ~ShaderHotReload();
        // Main-thread API; the shared executor must outlive this watcher.
        void Initialize(const std::filesystem::path &projectRoot, VulkanRenderer *renderer, JobExecutor &jobs);
        void Initialize(const std::filesystem::path &projectRoot, const std::filesystem::path &shaderRoot,
            ShaderReloadBackend backend, JobExecutor &jobs);
        void Shutdown();
        void Update();
        bool Enabled() const { return mEnabled; }
        const std::string &LastDiagnostic() const { return mLastDiagnostic; }
        uint64_t LastRevision() const { return mLastRevision; }

    private:
        struct Job
        {
            ShaderManifest manifest;
            std::filesystem::path directory;
            uint64_t parent = 0;
            uint64_t revision = 0;
            uint64_t materialEpoch = 0;
            std::vector<ShaderManifestOutput> outputs;
            std::vector<ShaderGenerationFile> inputs;
            std::vector<ShaderArtifact> artifacts;
            std::vector<std::string> changedSources;
            std::string catalogText;
            std::string materialName;
            std::string error;
            bool superseded = false;
        };
        struct Compilation
        {
            Job job;
            std::atomic_bool complete{false};
        };
        bool RefreshManifest();
        void RebuildDependencies(const std::filesystem::path &directory);
        std::vector<std::filesystem::path> ApprovedRoots() const;
        void PollMaterials();
        bool ExpandMaterial(const std::filesystem::path &path, Job &job);
        void StartJob(Job job);
        void PollJob();
        bool PublishJob(Job &job);
        void Requeue(const Job &job);
        void DropReady();

        std::filesystem::path mProjectRoot, mShaderDir, mReadDirectory;
        ShaderReloadBackend mBackend;
        ShaderManifest mManifest;
        ShaderDependencyIndex mDependencies;
        std::set<std::string> mPendingSources, mPendingMaterials;
        std::map<std::string, ShaderGenerationFile> mMaterialFiles;
        bool mMaterialsScanned = false;
        std::chrono::steady_clock::time_point mPendingSince{};
        std::string mLastDiagnostic;
        uint64_t mLastRevision = 0, mLoadedGeneration = 0, mMaterialEpoch = 0;
        bool mEnabled = false, mPublishBusy = false;
        std::atomic_bool mCancel{false};
        std::unique_ptr<BackgroundJobQueue> mQueue;
        std::shared_ptr<Compilation> mCompiling;
        std::optional<Job> mReady;
    };
}
