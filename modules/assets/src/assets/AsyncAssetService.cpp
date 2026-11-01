/**
 * @file AsyncAssetService.cpp
 * @author sumin.park
 * @brief Reads and decodes cooked assets on a background lane, and hands the results to the main thread.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "assets/AsyncAssetService.h"

#include "assets/AssetBlob.h"
#include "assets/AssetCache.h"
#include "assets/AssetsModule.h"

#include <core/fs/FileIO.h>
#include <core/log/Log.h>
#include <jobs/BackgroundJobQueue.h>
#include <jobs/JobExecutor.h>

#include <exception>
#include <filesystem>
#include <mutex>
#include <new>
#include <optional>
#include <utility>

namespace mir
{
    struct AsyncAssetService::Completions
    {
        struct Item
        {
            uint32_t index = 0;
            uint32_t generation = 0;
            std::shared_ptr<const void> result;
            std::string error;
        };

        void Push(Item item)
        {
            std::lock_guard lock(mutex);
            items.push_back(std::move(item));
        }

        std::mutex mutex;
        std::vector<Item> items;
    };

    // Everything a worker needs, copied from the manifest when the request was made
    struct AsyncAssetService::Job
    {
        struct Blob
        {
            AssetId id;
            std::filesystem::path path;
            uint32_t typeTag = 0;
            uint32_t contentVersion = 0;
        };

        // the root first
        std::vector<Blob> blobs;
        ErasedDecode decode;
        std::shared_ptr<Completions> completions;
        uint32_t index = 0;
        uint32_t generation = 0;
    };

    AsyncAssetService::AsyncAssetService(AssetsModule &assets, JobExecutor &jobs, std::size_t concurrency)
        : mAssets(assets),
        mJobs(jobs),
        mConcurrency(concurrency),
        mQueue(std::make_unique<BackgroundJobQueue>(jobs, concurrency)),
        mCompletions(std::make_shared<Completions>()),
        mSeenReloadEpoch(assets.ReloadEpoch())
    {}

    // the queue waits for its running jobs when destroyed
    AsyncAssetService::~AsyncAssetService() = default;

    AssetRequest AsyncAssetService::RequestErased(AssetId id, const void *descriptor, std::type_index type,
        std::span<const uint32_t> dependencyTags, ErasedDecode decode)
    {
        SyncReloadEpoch();

        const DedupKey key{ id.value, descriptor };
        if (const auto found = mDedup.find(key); found != mDedup.end())
            return { found->second, mGeneration };

        const uint32_t index = static_cast<uint32_t>(mSlots.size());
        mSlots.emplace_back(id, type);
        mDedup.emplace(key, index);
        ++mQueuedCount;

        const AssetManifest &manifest = mAssets.Manifest();
        std::vector<AssetId> closure;
        if (manifest.Find(id) != nullptr)
            closure = dependencyTags.empty() ? std::vector<AssetId>{ id } : manifest.Closure(id, dependencyTags);

        // a failure found here is published at the next drain like any other
        if (closure.empty())
        {
            mCompletions->Push({ index, mGeneration, nullptr, "it has no manifest entry" });
            return { index, mGeneration };
        }

        Job job;
        job.decode = std::move(decode);
        job.completions = mCompletions;
        job.index = index;
        job.generation = mGeneration;
        for (const AssetId blobId : closure)
        {
            const AssetManifestEntry *entry = manifest.Find(blobId);
            const std::optional<std::filesystem::path> path = mAssets.Cache().ResolvedPath(blobId);
            job.blobs.push_back({ blobId, path.value_or(std::filesystem::path{}), entry->typeTag, entry->contentVersion });
        }

        mQueue->Post([job = std::move(job)]() mutable { RunJob(job); });
        return { index, mGeneration };
    }

    void AsyncAssetService::RunJob(Job &job) noexcept
    {
        Completions::Item item{ job.index, job.generation, nullptr,{} };

        // each entry keeps its bytes, and its view points into them
        std::unordered_map<uint64_t, AssetCacheEntry> loaded;
        for (const Job::Blob &blob : job.blobs)
        {
            std::optional<std::vector<std::byte>> raw = ReadBytes(blob.path);
            if (!raw.has_value())
            {
                item.error = "cannot read " + blob.path.string();
                break;
            }
            const std::optional<AssetBlobView> view = ParseAssetBlob(*raw);
            if (!view.has_value())
            {
                item.error = blob.path.string() + " is not a valid blob";
                break;
            }
            if (view->header.typeTag != blob.typeTag || view->header.contentVersion != blob.contentVersion)
            {
                item.error = blob.path.string() + " does not match its manifest entry";
                break;
            }
            loaded.emplace(blob.id.value, AssetCacheEntry{ std::move(*raw), *view });
        }

        if (item.error.empty())
        {
            const AssetDependencies dependencies([&loaded](AssetId id) -> const AssetBlobView *
                {
                    const auto found = loaded.find(id.value);
                    return found == loaded.end() ? nullptr : &found->second.view;
                });
            try
            {
                item.result = job.decode(loaded.at(job.blobs.front().id.value).view, dependencies, item.error);
            }
            catch (const std::bad_alloc &)
            {
                // out of memory ends the process, it is not a failed load
                std::terminate();
            }
            catch (const std::exception &error)
            {
                item.result = nullptr;
                item.error = error.what();
            }
            if (!item.result && item.error.empty())
                item.error = "the decoder failed";
        }

        job.completions->Push(std::move(item));
    }

    AssetLoadState AsyncAssetService::State(AssetRequest request) const
    {
        if (request.generation != mGeneration || request.index >= mSlots.size())
            return AssetLoadState::Stale;
        return mSlots[request.index].state;
    }

    const std::string &AsyncAssetService::FailureReason(AssetRequest request) const
    {
        static const std::string none;
        return State(request) == AssetLoadState::Failed ? mSlots[request.index].error : none;
    }

    std::shared_ptr<const void> AsyncAssetService::AcquireErased(AssetRequest request, std::type_index type) const
    {
        if (State(request) != AssetLoadState::Ready)
            return nullptr;
        const Slot &slot = mSlots[request.index];
        return slot.type == type ? slot.result : nullptr;
    }

    void AsyncAssetService::DrainCompletions()
    {
        SyncReloadEpoch();

        std::vector<Completions::Item> items;
        {
            std::lock_guard lock(mCompletions->mutex);
            items.swap(mCompletions->items);
        }

        for (Completions::Item &item : items)
        {
            if (item.generation != mGeneration)
                continue;

            Slot &slot = mSlots[item.index];
            --mQueuedCount;
            if (item.result)
            {
                slot.result = std::move(item.result);
                slot.state = AssetLoadState::Ready;
            }
            else
            {
                slot.state = AssetLoadState::Failed;
                slot.error = item.error;
                MIR_LOG_ERROR("AsyncAssetService: asset {:016x} failed to load: {}", slot.id.value, item.error);
            }
        }
    }

    void AsyncAssetService::CancelAll()
    {
        ++mGeneration;

        // loads that have not started are dropped, the running ones finish and push results
        // for the old generation, which the clear below discards
        mQueue->CloseAndWait();
        mSlots.clear();
        mDedup.clear();
        mQueuedCount = 0;
        {
            std::lock_guard lock(mCompletions->mutex);
            mCompletions->items.clear();
        }

        // a closed queue drops every post, so the lane is replaced
        mQueue = std::make_unique<BackgroundJobQueue>(mJobs, mConcurrency);
    }

    bool AsyncAssetService::Idle() const
    {
        return mQueuedCount == 0;
    }

    void AsyncAssetService::SyncReloadEpoch()
    {
        if (mAssets.ReloadEpoch() == mSeenReloadEpoch)
            return;
        mSeenReloadEpoch = mAssets.ReloadEpoch();
        CancelAll();
    }
}
