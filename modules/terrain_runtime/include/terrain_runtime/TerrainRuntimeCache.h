/**
 * @file TerrainRuntimeCache.h
 * @author Jackey Cen
 * @brief The job pool, payload cache and GPU meshes the Edit and Play terrain runtimes share.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <terrain/TerrainExecutor.h>
#include <terrain/TerrainIdentity.h>
#include <terrain/TerrainMeshResidency.h>
#include <terrain/TerrainStreamer.h>
#include <terrain/TerrainStreamingSettings.h>

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <vector>

namespace mir
{
    class JobExecutor;
    class TerrainGpuUploader;
    class TerrainJobExecutor;
    class VulkanRenderer;

    // One runtime holding the cache; each World keeps its own colliders and release switch.
    using TerrainCacheOwner = uint64_t;

    // Main-thread object. Play binds to the cache Edit built, so entering Play finds the payloads and meshes the
    // Edit runtime already holds and one GPU budget covers the whole App.
    class TerrainRuntimeCache
    {
      public:
        TerrainRuntimeCache(const TerrainStreamingSettings &settings, JobExecutor &jobs, VulkanRenderer &renderer);
        // Over an executor and an uploader the caller owns, such as a manual executor and a fake uploader.
        TerrainRuntimeCache(const TerrainStreamingSettings &settings, std::shared_ptr<TerrainExecutor> executor,
                            std::shared_ptr<TerrainMeshUploader> uploader);
        ~TerrainRuntimeCache();

        TerrainRuntimeCache(const TerrainRuntimeCache &) = delete;
        TerrainRuntimeCache &operator=(const TerrainRuntimeCache &) = delete;

        // Finishes the work this cache posted before its collaborators go.
        void WaitIdle();
        size_t Concurrency() const;

        TerrainCacheOwner AddOwner();
        // The releases this owner holds. Meshes of a release no owner holds retire now.
        void SetHeldReleases(TerrainCacheOwner owner, std::span<const TerrainReleaseInfo> releases);
        void RemoveOwner(TerrainCacheOwner owner);

        // One clock for both runtimes, the streamer and the mesh residency.
        double Now() const;
        TerrainStreamer &Streamer();
        TerrainMeshResidency &Meshes();
        std::shared_ptr<TerrainExecutor> Executor() const;
        // Meshes built with a BLAS only when the renderer traces; a change retires the ones built the other way.
        bool RayTracing() const
        {
            return mRayTracing;
        }
        void SetRayTracing(bool rayTracing);

      private:
        void MakeResidencies(const TerrainStreamingSettings &settings);
        void RetireUnheld();

        std::chrono::steady_clock::time_point mStart = std::chrono::steady_clock::now();
        std::shared_ptr<TerrainJobExecutor> mJobPool;
        std::shared_ptr<TerrainExecutor> mPool;
        std::shared_ptr<TerrainMeshUploader> mUploader;
        // The renderer's uploader, when this cache made one.
        TerrainGpuUploader *mGpuUploader = nullptr;
        std::unique_ptr<TerrainStreamer> mStreamer;
        std::unique_ptr<TerrainMeshResidency> mMeshes;
        // By owner: the releases whose payloads and meshes stay.
        std::map<TerrainCacheOwner, std::vector<TerrainReleaseInfo>> mHeld;
        TerrainCacheOwner mNextOwner = 1;
        bool mRayTracing = false;
    };
}
