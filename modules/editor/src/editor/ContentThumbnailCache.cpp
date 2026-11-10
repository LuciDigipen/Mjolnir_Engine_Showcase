/**
 * @file ContentThumbnailCache.cpp
 * @author sumin.park
 * @brief Visibility-driven thumbnail requests and bounded GPU caching.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "editor/ContentThumbnailCache.h"
#include "editor/TextureThumbnailDecoder.h"

#include <core/log/Log.h>
#include <jobs/BackgroundJobQueue.h>

#include <algorithm>
#include <vector>

namespace mir
{
    namespace
    {
        uint64_t Content(AsyncAssetService *assets, AssetId id)
        {
            return UploadContent(assets, id) ^ (uint64_t(kThumbnailGeneratorVersion) << 32) ^ kThumbnailSize;
        }
    }
    ContentThumbnailCache::ContentThumbnailCache(AsyncAssetService *assets, ITextureUploader &uploader, JobExecutor *jobs)
        : mAssets(assets), mUploader(uploader), mResidency(uploader.SharedResidency())
    {
        if (jobs)
            mSourceJobs = std::make_unique<BackgroundJobQueue>(*jobs, 1);
        if (mResidency)
        {
            mResidencyHolder = mResidency->AddHolder(GpuCategory::Texture, [this] { return HeldBytes(); });
            mResidency->SetRetiring(GpuCategory::Texture, [&uploader] { return uploader.TextureRetiringBytes(); });
        }
        mSlotReclaimer = mUploader.AddTextureSlotReclaimer([this]
            {
                if (mUploader.TextureSlotsAvailable() == 0)
                    EvictUnused();
            });
    }
    ContentThumbnailCache::~ContentThumbnailCache()
    {
        mSlotReclaimer.Reset();
        Clear();
        if (mResidency)
            mResidency->RemoveHolder(mResidencyHolder);
        if (mSourceJobs)
            mSourceJobs->CloseAndWait();
    }

    void ContentThumbnailCache::Remove(uint64_t id)
    {
        auto found = mEntries.find(id);
        if (found == mEntries.end())
            return;
        if (found->second.mCancelled)
            *found->second.mCancelled = true;
        if (!found->second.mPreview.mTexture.IsNull())
            mUploader.RetireTexture(found->second.mPreview.mTexture);
        mEntries.erase(found);
    }

    uint64_t ContentThumbnailCache::HeldBytes() const
    {
        uint64_t bytes = 0;
        for (const auto &[id, entry] : mEntries)
            if (!entry.mPreview.mTexture.IsNull())
                bytes += mUploader.TextureFootprint(entry.mPreview.mTexture);
        return bytes;
    }

    bool ContentThumbnailCache::EvictUnused()
    {
        auto oldest = mEntries.end();
        for (auto it = mEntries.begin(); it != mEntries.end(); ++it)
            if (!mVisible.contains(it->first) && !it->second.mPreview.mTexture.IsNull() &&
                mUploader.TextureState(it->second.mPreview.mTexture) == TextureUploadState::Ready &&
                (oldest == mEntries.end() || it->second.mUse < oldest->second.mUse))
                oldest = it;
        if (oldest == mEntries.end())
            return false;
        Remove(oldest->first);
        return true;
    }

    void ContentThumbnailCache::Clear()
    {
        while (!mEntries.empty())
            Remove(mEntries.begin()->first);
        mFailures.clear();
    }

    void ContentThumbnailCache::BeginFrame(size_t capacity)
    {
        mCapacity = std::min(capacity, kMaxThumbnails);
        mVisible.clear();
        mSources.clear();
        if (mAssets && mRevision != mAssets->Revision())
        {
            mRevision = mAssets->Revision();
            mFailures.clear();
            std::vector<uint64_t> obsolete;
            for (const auto &[id, entry] : mEntries)
                if (entry.mContent != Content(mAssets, AssetId{ id }))
                    obsolete.push_back(id);
            for (uint64_t id : obsolete)
                Remove(id);
        }
    }

    ContentThumbnail ContentThumbnailCache::Request(AssetId id, const std::filesystem::path &source)
    {
        if (!mAssets || id.IsNull() || mCapacity == 0)
            return {};
        // Excess visible cells keep their type icon rather than thrashing the cache.
        if (!mVisible.contains(id.value) && mVisible.size() >= mCapacity)
            return {};
        mVisible.insert(id.value);
        if (!source.empty())
            mSources[id.value] = source;
        const auto found = mEntries.find(id.value);
        if (found == mEntries.end())
            return {};
        auto &entry = found->second;
        entry.mUse = ++mSerial;
        if (!entry.mPreview.mTexture.IsNull() &&
            mUploader.TextureState(entry.mPreview.mTexture) == TextureUploadState::Ready)
            return entry.mPreview;
        return {};
    }

    void ContentThumbnailCache::EndFrame()
    {
        std::vector<uint64_t> remove;
        for (const auto &[id, entry] : mEntries)
            if (!mVisible.contains(id) && (entry.mPreview.mTexture.IsNull() ||
                mUploader.TextureState(entry.mPreview.mTexture) != TextureUploadState::Ready))
                remove.push_back(id);
        for (auto id : remove)
            Remove(id);
        while ((mEntries.size() > mCapacity ||
            (mResidency && mResidency->Excess(GpuCategory::Texture) != 0)) && EvictUnused()) {}
        if (!mAssets)
            return;
        size_t active = 0;
        for (const auto &[id, entry] : mEntries)
            if (entry.mPreview.mTexture.IsNull())
                ++active;
        size_t admissions = 0, uploads = 0;
        for (auto id : mVisible)
        {
            const uint64_t content = Content(mAssets, AssetId{ id });
            if (const auto failure = mFailures.find(id); failure != mFailures.end() && failure->second == content)
                continue;
            auto found = mEntries.find(id);
            if (found == mEntries.end())
            {
                // Admit a small amount only while the scene asset lane is idle.
                if (active >= 2 || admissions >= 1 || !mAssets->Idle())
                    continue;
                while (mEntries.size() >= mCapacity && EvictUnused()) {}
                if (mEntries.size() >= mCapacity)
                    continue;
                auto &entry = mEntries[id];
                entry.mContent = content;
                entry.mUse = ++mSerial;
                if (const auto source = mSources.find(id); source != mSources.end() && mSourceJobs)
                {
                    entry.mCancelled = std::make_shared<std::atomic<bool>>(false);
                    entry.mSource = mSourceJobs->Submit([cancelled = entry.mCancelled, path = source->second]() noexcept
                        {
                            SourceResult result;
                            if (!cancelled->load())
                                result.mSource = LoadSourceThumbnail(path, result.mError);
                            return result;
                        });
                }
                else
                    entry.mRequest = mAssets->RequestLease(AssetId{ id }, TextureThumbnailDecoder());
                ++active;
                ++admissions;
                found = mEntries.find(id);
            }
            auto &entry = found->second;
            if (!entry.mPreview.mTexture.IsNull())
            {
                const auto state = mUploader.TextureState(entry.mPreview.mTexture);
                if (state == TextureUploadState::Failed || state == TextureUploadState::Stale)
                {
                    if (state == TextureUploadState::Failed)
                    {
                        MIR_LOG_WARN("Thumbnail {:016x}: GPU upload failed", id);
                        mFailures[id] = content;
                    }
                    Remove(id);
                }
                continue;
            }
            if (uploads >= 2 || mUploader.TextureSlotsAvailable() <= kSceneSlotReserve)
                continue;
            std::shared_ptr<const TextureUploadSource> source;
            if (entry.mSource.valid())
            {
                if (entry.mSource.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
                    continue;
                auto result = entry.mSource.get();
                if (!result.mSource.desc.width)
                {
                    MIR_LOG_WARN("Thumbnail {:016x}: {}", id, result.mError);
                    mFailures[id] = content;
                    Remove(id);
                    continue;
                }
                source = std::make_shared<TextureUploadSource>(std::move(result.mSource));
            }
            else
            {
                const auto state = entry.mRequest.State();
                if (state == AssetLoadState::Failed || state == AssetLoadState::Stale)
                {
                    if (state == AssetLoadState::Failed)
                        mFailures[id] = content;
                    Remove(id);
                    continue;
                }
                if (state != AssetLoadState::Ready)
                    continue;
                source = entry.mRequest.Acquire();
            }
            {
                const auto handle = mUploader.QueueTexture({ source->desc, source });
                entry.mRequest.Reset();
                if (handle.IsNull())
                {
                    MIR_LOG_WARN("Thumbnail {:016x}: upload could not be queued", id);
                    mFailures[id] = content;
                    Remove(id);
                }
                else
                    entry.mPreview = { handle, source->desc.width, source->desc.height };
                ++uploads;
            }
        }
    }
}
