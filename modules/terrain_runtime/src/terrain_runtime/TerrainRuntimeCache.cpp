/**
 * @file TerrainRuntimeCache.cpp
 * @author Jackey Cen
 * @brief The job pool, payload cache, GPU meshes and weight textures the Edit and Play terrain runtimes share.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <terrain_runtime/TerrainRuntimeCache.h>

#include <terrain_runtime/TerrainJobExecutor.h>
#include <terrain_runtime/TerrainRender.h>
#include <terrain_runtime/TerrainWeightResidency.h>

#include <jobs/JobExecutor.h>
#include <renderer/VulkanRenderer.h>

#include <utility>

namespace mir
{
    TerrainRuntimeCache::TerrainRuntimeCache(const TerrainStreamingSettings &settings, JobExecutor &jobs,
                                             VulkanRenderer &renderer)
        : mJobPool(std::make_shared<TerrainJobExecutor>(jobs, settings.io.workerThreads)),
          mPool(mJobPool),
          mUploader(std::make_shared<TerrainGpuUploader>(renderer))
    {
        mGpuUploader = static_cast<TerrainGpuUploader *>(mUploader.get());
        const SamplerDesc weightSampler{.magFilter = VK_FILTER_LINEAR,
                                        .minFilter = VK_FILTER_LINEAR,
                                        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
        MakeResidencies(settings, &renderer, renderer.MaterialSamplerRow(weightSampler));
    }

    TerrainRuntimeCache::TerrainRuntimeCache(const TerrainStreamingSettings &settings,
                                             std::shared_ptr<TerrainExecutor> executor,
                                             std::shared_ptr<TerrainMeshUploader> uploader,
                                             ITextureUploader *textures, uint32_t samplerRow)
        : mPool(std::move(executor)), mUploader(std::move(uploader))
    {
        MakeResidencies(settings, textures, samplerRow);
    }

    TerrainRuntimeCache::~TerrainRuntimeCache()
    {
        // Nothing of this cache may still run once the streamer and the residencies are gone.
        WaitIdle();
        mWeights.reset();
        mMeshes.reset();
        mStreamer.reset();
        mUploader.reset();
    }

    void TerrainRuntimeCache::MakeResidencies(const TerrainStreamingSettings &settings, ITextureUploader *textures,
                                              uint32_t samplerRow)
    {
        mStreamer = std::make_unique<TerrainStreamer>(settings, mPool, [this] { return Now(); });
        mMeshes = std::make_unique<TerrainMeshResidency>(settings.gpu, mPool, *mUploader);
        if (textures)
            mWeights = std::make_unique<TerrainWeightResidency>(settings.gpu, mPool, *textures, samplerRow);
    }

    void TerrainRuntimeCache::WaitIdle()
    {
        if (mJobPool)
            mJobPool->WaitIdle();
    }

    size_t TerrainRuntimeCache::Concurrency() const
    {
        return mJobPool ? mJobPool->Concurrency() : 0;
    }

    void TerrainRuntimeCache::SetRayTracing(bool rayTracing)
    {
        mRayTracing = rayTracing;
        if (mGpuUploader)
            mGpuUploader->SetRayTracing(rayTracing);
    }

    TerrainCacheOwner TerrainRuntimeCache::AddOwner()
    {
        return mNextOwner++;
    }

    void TerrainRuntimeCache::SetHeldReleases(TerrainCacheOwner owner, std::span<const TerrainReleaseInfo> releases)
    {
        mHeld[owner].assign(releases.begin(), releases.end());
        RetireUnheld();
    }

    void TerrainRuntimeCache::RemoveOwner(TerrainCacheOwner owner)
    {
        if (mHeld.erase(owner) > 0)
            RetireUnheld();
    }

    void TerrainRuntimeCache::RetireUnheld()
    {
        std::vector<TerrainReleaseInfo> keep;
        for (const auto &[owner, releases] : mHeld)
            keep.insert(keep.end(), releases.begin(), releases.end());
        mMeshes->RetireReleases(keep);
        // A weight texture names no release, so one the other runtime may draw is left to expire.
        if (mWeights && keep.empty())
            mWeights->RetireAll();
    }

    double TerrainRuntimeCache::Now() const
    {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - mStart).count();
    }

    TerrainStreamer &TerrainRuntimeCache::Streamer()
    {
        return *mStreamer;
    }

    TerrainMeshResidency &TerrainRuntimeCache::Meshes()
    {
        return *mMeshes;
    }

    TerrainWeightResidency *TerrainRuntimeCache::Weights() const
    {
        return mWeights.get();
    }

    std::shared_ptr<TerrainExecutor> TerrainRuntimeCache::Executor() const
    {
        return mPool;
    }
}
