/**
 * @file TerrainRuntimeCache.h
 * @author Jackey Cen
 * @brief The job pool, payload cache, GPU meshes and weight textures the Edit and Play terrain runtimes share.
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
    class ITextureUploader;
    class JobExecutor;
    class TerrainGpuUploader;
    class TerrainJobExecutor;
    class TerrainWeightResidency;
    class VulkanRenderer;

    // One runtime holding the cache; each World keeps its own colliders and release switch.
    using TerrainCacheOwner = uint64_t;

    // Main-thread object. Play binds to the cache Edit built, so entering Play finds the payloads, meshes and weight
    // textures the Edit runtime already holds and one budget of each covers the whole App.
    class TerrainRuntimeCache
    {
      public:
        TerrainRuntimeCache(const TerrainStreamingSettings &settings, JobExecutor &jobs, VulkanRenderer &renderer);
        // Over an executor and uploaders the caller owns, such as a manual executor and fakes.
        TerrainRuntimeCache(const TerrainStreamingSettings &settings, std::shared_ptr<TerrainExecutor> executor,
                            std::shared_ptr<TerrainMeshUploader> uploader, ITextureUploader *textures = nullptr,
                            uint32_t samplerRow = UINT32_MAX);
        ~TerrainRuntimeCache();

        TerrainRuntimeCache(const TerrainRuntimeCache &) = delete;
        TerrainRuntimeCache &operator=(const TerrainRuntimeCache &) = delete;

        // Finishes the work this cache posted before its collaborators go.
        void WaitIdle();
        size_t Concurrency() const;

        TerrainCacheOwner AddOwner();
        // The releases this owner holds. Meshes of a release no owner holds retire now, and every weight texture
        // once no owner holds any.
        void SetHeldReleases(TerrainCacheOwner owner, std::span<const TerrainReleaseInfo> releases);
        void RemoveOwner(TerrainCacheOwner owner);

        // One clock for both runtimes, the streamer and the mesh residency.
        double Now() const;
        TerrainStreamer &Streamer();
        TerrainMeshResidency &Meshes();
        // Null without a texture uploader.
        TerrainWeightResidency *Weights() const;
        std::shared_ptr<TerrainExecutor> Executor() const;
        // Meshes built with a BLAS only when the renderer traces; a change retires the ones built the other way.
        bool RayTracing() const
        {
            return mRayTracing;
        }
        void SetRayTracing(bool rayTracing);

      private:
        void MakeResidencies(const TerrainStreamingSettings &settings, ITextureUploader *textures,
                             uint32_t samplerRow);
        void RetireUnheld();

        std::chrono::steady_clock::time_point mStart = std::chrono::steady_clock::now();
        std::shared_ptr<TerrainJobExecutor> mJobPool;
        std::shared_ptr<TerrainExecutor> mPool;
        std::shared_ptr<TerrainMeshUploader> mUploader;
        // The renderer's uploader, when this cache made one.
        TerrainGpuUploader *mGpuUploader = nullptr;
        std::unique_ptr<TerrainStreamer> mStreamer;
        std::unique_ptr<TerrainMeshResidency> mMeshes;
        std::unique_ptr<TerrainWeightResidency> mWeights;
        // By owner: the releases whose payloads and meshes stay.
        std::map<TerrainCacheOwner, std::vector<TerrainReleaseInfo>> mHeld;
        TerrainCacheOwner mNextOwner = 1;
        bool mRayTracing = false;
    };
}
