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
#include <algorithm>
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
            std::shared_ptr<detail::AssetLoadRecord> record;
            AssetId id;
            uint64_t bytes = 0;
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
        std::shared_ptr<detail::AssetLoadRecord> record;
        AssetId id;
        std::shared_ptr<const AssetManifest> catalog;
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
    AsyncAssetService::~AsyncAssetService()
    {
        for (auto &job : mPending)
        {
            job->record->mCancelled = true;
            job->record->mState = AssetLoadState::Stale;
        }
        mPending.clear();
        mQueue->WaitIdle();
        DrainCompletions();
    }

    AssetRequest AsyncAssetService::RequestErased(AssetId id, const void *descriptor, std::type_index type,
        std::span<const uint32_t> dependencyTags, ErasedDecode decode, bool pinned)
    {
        SyncReloadEpoch();

        const DedupKey key{ id.value, descriptor };
        if (const auto found = mDedup.find(key); found != mDedup.end())
        {
            auto &record = mSlots[found->second].record;
            if (!record->mCancelled.load())
            {
                record->mPinned |= pinned;
                record->mLastUse = ++mUseSerial;
                return { found->second, mGeneration };
            }
            record->mState = AssetLoadState::Stale;
            record.reset();
            mFreeSlots.push_back(found->second);
            mDedup.erase(found);
        }

        const auto catalog = mAssets.ManifestSnapshot();
        const AssetManifest &manifest = *catalog;
        const uint64_t content = dependencyTags.empty() ? manifest.EntryContent(id) : manifest.ClosureContent(id, dependencyTags);

        uint32_t index;
        if (mFreeSlots.empty())
        {
            index = static_cast<uint32_t>(mSlots.size());
            mSlots.emplace_back(id, type);
        }
        else
        {
            index = mFreeSlots.back();
            mFreeSlots.pop_back();
            mSlots[index] = Slot(id, type);
        }
        mSlots[index].record->mPinned = pinned;
        mSlots[index].record->mLastUse = ++mUseSerial;
        mSlots[index].content = content;
        mSlots[index].descriptor = descriptor;
        mSlots[index].record->mContent = content;
        mSlots[index].record->mDescriptor = descriptor;
        mDedup.emplace(key, index);

        // Reuse live decoded data when the files have not changed.
        if (content != 0)
            if (const auto carried = std::find_if(mCarried.begin(), mCarried.end(),
                    [&](const Carried &item) { return item.key == key && item.content == content && item.type == type; });
                carried != mCarried.end())
                if (auto result = carried->result.lock())
                {
                    auto &record = *mSlots[index].record;
                    record.mResult = std::move(result);
                    record.mBytes = carried->bytes;
                    record.mState = AssetLoadState::Ready;
                    return { index, mGeneration };
                }
        ++mQueuedCount;

        std::vector<AssetId> closure;
        if (manifest.Find(id) != nullptr)
            closure = dependencyTags.empty() ? std::vector<AssetId>{ id } : manifest.Closure(id, dependencyTags);

        // Even immediate failures wait for DrainCompletions, keeping request timing consistent.
        if (closure.empty())
        {
            mCompletions->Push({ index, mGeneration, nullptr, "it has no manifest entry", mSlots[index].record, id });
            return { index, mGeneration };
        }

        Job job;
        job.decode = std::move(decode);
        job.completions = mCompletions;
        job.index = index;
        job.generation = mGeneration;
        job.record = mSlots[index].record;
        job.id = id;
        job.catalog = catalog;
        for (const AssetId blobId : closure)
        {
            const AssetManifestEntry *entry = manifest.Find(blobId);
            const std::optional<std::filesystem::path> path = mAssets.Cache().ResolvedPath(blobId);
            job.blobs.push_back({ blobId, path.value_or(std::filesystem::path{}), entry->typeTag, entry->contentVersion });
        }

        mPending.push_back(std::make_unique<Job>(std::move(job)));
        DispatchPending();
        return { index, mGeneration };
    }

    void AsyncAssetService::DispatchPending()
    {
        while (!mPending.empty() && (mJobs.WorkerCount() == 0 || mActiveJobs < mConcurrency))
        {
            // Under decoded-memory pressure, admit one required job at a time so dependencies
            // can still make progress. Zero-worker mode preserves its inline contract.
            if (mJobs.WorkerCount() != 0 && mActiveJobs != 0 && DecodedBytes() >= mDecodedBudget)
                break;
            auto job = std::move(mPending.front());
            mPending.pop_front();
            if (job->record->mCancelled.load())
            {
                job->record->mState = AssetLoadState::Stale;
                if (job->generation == mGeneration)
                    --mQueuedCount;
                continue;
            }
            ++mActiveJobs;
            job->record->mDispatched = true;
            // The callback is copyable for the executor, the service retains no job payload.
            auto shared = std::shared_ptr<Job>(std::move(job));
            mQueue->Post([shared] { RunJob(*shared); });
        }
    }

    void AsyncAssetService::RunJob(Job &job) noexcept
    {
        Completions::Item item{ job.index, job.generation, nullptr,{}, job.record, job.id };
        if (job.record->mCancelled.load())
        {
            job.completions->Push(std::move(item));
            return;
        }

        try
        {
            // each entry keeps its bytes, and its view points into them
            std::unordered_map<uint64_t, AssetCacheEntry> loaded;
            for (const Job::Blob &blob : job.blobs)
            {
                if (job.record->mCancelled.load())
                    break;
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

            if (item.error.empty() && !job.record->mCancelled.load())
            {
                const AssetDependencies dependencies([&loaded](AssetId id) -> const AssetBlobView *
                    {
                        const auto found = loaded.find(id.value);
                        return found == loaded.end() ? nullptr : &found->second.view;
                    });
                auto decoded = job.decode(loaded.at(job.blobs.front().id.value).view, dependencies, item.error);
                item.result = std::move(decoded.value);
                item.bytes = decoded.bytes;
                if (!item.result && item.error.empty())
                    item.error = "the decoder failed";
            }

        }
        catch (const std::bad_alloc &)
        {
            std::terminate();
        }
        catch (const std::exception &error)
        {
            item.result.reset();
            item.error = error.what();
        }
        catch (...)
        {
            item.result.reset();
            item.error = "the asset worker threw an unknown exception";
        }
        job.completions->Push(std::move(item));
    }

    AssetLoadState AsyncAssetService::State(AssetRequest request) const
    {
        if (request.generation != mGeneration || request.index >= mSlots.size())
            return AssetLoadState::Stale;
        const auto &record = mSlots[request.index].record;
        return record ? record->mState : AssetLoadState::Stale;
    }

    const std::string &AsyncAssetService::FailureReason(AssetRequest request) const
    {
        static const std::string none;
        return State(request) == AssetLoadState::Failed ? mSlots[request.index].record->mError : none;
    }

    std::shared_ptr<const void> AsyncAssetService::AcquireErased(AssetRequest request, std::type_index type) const
    {
        if (State(request) != AssetLoadState::Ready)
            return nullptr;
        const auto &record = *mSlots[request.index].record;
        return record.mType == type ? record.mResult : nullptr;
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
            // A missing manifest entry produces a completion without dispatching a worker.
            if (item.record->mDispatched)
                --mActiveJobs;
            if (item.generation == mGeneration)
                --mQueuedCount;
            auto &record = *item.record;
            if (record.mCancelled.load() || (item.generation != mGeneration && record.mConsumers == 0))
            {
                record.mState = AssetLoadState::Stale;
                continue;
            }
            if (item.result)
            {
                record.mResult = std::move(item.result);
                record.mBytes = item.bytes;
                mAllocations.push_back({ record.mResult, item.bytes });
                record.mState = AssetLoadState::Ready;
                if (record.mContent != 0)
                {
                    const DedupKey key{ item.id.value, record.mDescriptor };
                    std::erase_if(mCarried, [&](const Carried &live)
                        { return live.result.expired() || (live.key == key && live.content == record.mContent); });
                    mCarried.push_back({ key, record.mContent, record.mType, record.mResult, item.bytes });
                }
            }
            else
            {
                record.mState = AssetLoadState::Failed;
                record.mError = item.error;
                MIR_LOG_ERROR("AsyncAssetService: asset {:016x} failed to load: {}", item.id.value, item.error);
            }
        }
        TrimDecoded();
        DispatchPending();
    }

    void AsyncAssetService::CancelAll()
    {
        ++mGeneration;

        for (Slot &slot : mSlots)
            if (slot.record)
            {
                slot.record->mCancelled = true;
                slot.record->mState = AssetLoadState::Stale;
                slot.record->mResult.reset();
            }
        mSlots.clear();
        mFreeSlots.clear();
        mDedup.clear();
        mPending.clear();
        mQueuedCount = 0;
    }

    bool AsyncAssetService::Idle() const
    {
        return mQueuedCount == 0;
    }

    uint64_t AsyncAssetService::ContentOfTags(AssetId id, std::span<const uint32_t> tags) const
    {
        const auto catalog = mAssets.ManifestSnapshot();
        return tags.empty() ? catalog->EntryContent(id) : catalog->ClosureContent(id, tags);
    }

    std::shared_ptr<const AssetManifest> AsyncAssetService::CatalogSnapshot() const
    {
        return mAssets.ManifestSnapshot();
    }

    uint32_t AsyncAssetService::Revision() const { return mAssets.ReloadEpoch(); }

    uint64_t AsyncAssetService::DecodedBytes() const
    {
        uint64_t bytes = 0;
        for (const Allocation &allocation : mAllocations)
            if (!allocation.value.expired())
                bytes += allocation.bytes;
        return bytes;
    }

    void AsyncAssetService::SetDecodedBudget(uint64_t bytes)
    {
        mDecodedBudget = bytes;
        TrimDecoded();
    }

    void AsyncAssetService::TrimDecoded()
    {
        std::erase_if(mPending, [this](const std::unique_ptr<Job> &job)
            {
                if (!job->record->mCancelled.load())
                    return false;
                job->record->mState = AssetLoadState::Stale;
                if (job->generation == mGeneration)
                    --mQueuedCount;
                return true;
            });
        std::erase_if(mAllocations, [](const Allocation &allocation) { return allocation.value.expired(); });
        for (auto it = mDedup.begin(); it != mDedup.end();)
        {
            auto &record = mSlots[it->second].record;
            if (!record->mCancelled.load() || record->mPinned || record->mConsumers != 0)
            {
                ++it;
                continue;
            }
            record.reset();
            mFreeSlots.push_back(it->second);
            it = mDedup.erase(it);
        }
        std::vector<std::pair<uint64_t, DedupKey>> unused;
        for (const auto &[key, index] : mDedup)
        {
            const auto &record = *mSlots[index].record;
            if (!record.mPinned && record.mConsumers == 0 && record.mState != AssetLoadState::Queued)
                unused.emplace_back(record.mLastUse, key);
        }
        std::sort(unused.begin(), unused.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
        for (const auto &[serial, key] : unused)
        {
            if (DecodedBytes() <= mDecodedBudget)
                break;
            const auto found = mDedup.find(key);
            mSlots[found->second].record.reset();
            mFreeSlots.push_back(found->second);
            mDedup.erase(found);
        }
    }

    void AsyncAssetService::SyncReloadEpoch()
    {
        if (mAssets.ReloadEpoch() == mSeenReloadEpoch)
            return;
        mSeenReloadEpoch = mAssets.ReloadEpoch();
        // Existing leases keep their data, new requests use the new manifest.
        ++mGeneration;
        for (Slot &slot : mSlots)
            if (slot.record && slot.record->mConsumers == 0)
                slot.record->mCancelled = true;
        mSlots.clear();
        mFreeSlots.clear();
        mDedup.clear();
        mQueuedCount = 0;
    }

    std::size_t AsyncAssetService::EvictUnused()
    {
        std::size_t count = 0;
        for (auto it = mDedup.begin(); it != mDedup.end();)
        {
            auto &record = mSlots[it->second].record;
            if (record->mConsumers != 0 || record->mPinned)
            {
                ++it;
                continue;
            }
            record->mCancelled = true;
            record->mState = AssetLoadState::Stale;
            record.reset();
            mFreeSlots.push_back(it->second);
            it = mDedup.erase(it);
            ++count;
        }
        return count;
    }
}
