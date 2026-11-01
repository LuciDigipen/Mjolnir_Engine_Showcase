/**
 * @file AsyncAssetService.h
 * @author sumin.park
 * @brief Reads and decodes cooked assets on a background lane, and hands the results to the main thread.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "assets/AssetDecoder.h"
#include "assets/AssetId.h"
#include "assets/AssetLease.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <typeindex>
#include <unordered_map>
#include <vector>

namespace mir
{
    class AssetsModule;
    class BackgroundJobQueue;
    class JobExecutor;

    struct AssetRequest
    {
        uint32_t index = 0;
        uint32_t generation = 0;
    };

    // Loads and decodes cooked assets on a background lane. Main thread only, and a worker never
    // touches the manifest, the cache or the World.
    class AsyncAssetService
    {
    public:
        // concurrency must be positive, the job queue lowers it to the executor's worker count
        AsyncAssetService(AssetsModule &assets, JobExecutor &jobs, std::size_t concurrency);
        ~AsyncAssetService();

        AsyncAssetService(const AsyncAssetService &) = delete;
        AsyncAssetService &operator=(const AsyncAssetService &) = delete;

        // A handle for loading id with decoder. The same asset and decoder share one load and one
        // result, and a failure is logged once and remembered until CancelAll or a reload.
        template <class T>
        AssetRequest Request(AssetId id, const AssetDecoder<T> &decoder)
        {
            return RequestErased(id, &decoder, typeid(T), decoder.dependencyTags, MakeDecode(decoder), true);
        }

        // Independent claims share one decode. Leases and borrowed results may outlive the service.
        template <class T>
        AssetLease<T> RequestLease(AssetId id, const AssetDecoder<T> &decoder)
        {
            const AssetRequest request = RequestErased(id, &decoder, typeid(T), decoder.dependencyTags,
                MakeDecode(decoder), false);
            return AssetLease<T>(mSlots[request.index].record);
        }

        // Drops unused lease-managed entries. Outstanding borrowers remain valid.
        std::size_t EvictUnused();
        uint32_t Revision() const;
        uint64_t DecodedBytes() const;
        // Active claims and borrowers can exceed this target, only unused entries are evicted.
        void SetDecodedBudget(uint64_t bytes);

        AssetLoadState State(AssetRequest request) const;

        // convert failure to printable string
        const std::string &FailureReason(AssetRequest request) const;

        // The decoded result, or null unless the request is Ready and was made for this T. It stays
        // valid while the caller holds it, even past CancelAll.
        template <class T>
        std::shared_ptr<const T> Acquire(AssetRequest request) const
        {
            return std::static_pointer_cast<const T>(AcquireErased(request, typeid(T)));
        }

        // Makes finished loads visible, and a state changes nowhere else. Call once a frame, before
        // anything reads State.
        void DrainCompletions();

        // Invalidates requests without waiting. Queued callbacks skip cancelled records,
        // running workers finish safely. Only service destruction waits for workers.
        void CancelAll();

        // True when no request is waiting for a result
        bool Idle() const;

    private:
        struct DecodedResult
        {
            std::shared_ptr<const void> value;
            uint64_t bytes = 0;
        };
        using ErasedDecode = std::function<DecodedResult(
            const AssetBlobView &root, const AssetDependencies &dependencies, std::string &error)>;

        template <class T> static ErasedDecode MakeDecode(const AssetDecoder<T> &decoder)
        {
            return [decode = decoder.decode, size = decoder.sizeBytes](const AssetBlobView &root,
                const AssetDependencies &dependencies, std::string &error) -> DecodedResult
                {
                    auto value = std::make_shared<T>();
                    if (!decode(root, dependencies, *value, error))
                        return {};
                    return { value, size ? size(*value) : sizeof(T) };
                };
        }

        struct Completions;
        struct Job;

        struct Slot
        {
            Slot(AssetId assetId, std::type_index assetType)
                : id(assetId), record(std::make_shared<detail::AssetLoadRecord>(assetType)) {}

            AssetId id;
            std::shared_ptr<detail::AssetLoadRecord> record;
        };

        struct DedupKey
        {
            uint64_t id = 0;
            const void *descriptor = nullptr;
            bool operator==(const DedupKey &) const = default;
        };

        struct DedupKeyHash
        {
            std::size_t operator()(const DedupKey &key) const
            {
                return std::hash<uint64_t>{}(key.id) ^ (std::hash<const void *>{}(key.descriptor) * 0x9E3779B97F4A7C15ull);
            }
        };

        AssetRequest RequestErased(AssetId id, const void *descriptor, std::type_index type,
            std::span<const uint32_t> dependencyTags, ErasedDecode decode, bool pinned);
        std::shared_ptr<const void> AcquireErased(AssetRequest request, std::type_index type) const;
        static void RunJob(Job &job) noexcept;
        // CancelAll when a reload was committed since the last call
        void SyncReloadEpoch();
        void TrimDecoded();
        void DispatchPending();

        AssetsModule &mAssets;
        JobExecutor &mJobs;
        std::size_t mConcurrency;
        std::unique_ptr<BackgroundJobQueue> mQueue;
        std::shared_ptr<Completions> mCompletions;
        std::deque<std::unique_ptr<Job>> mPending;
        std::size_t mActiveJobs = 0;

        // starts at 1 so a default AssetRequest is Stale
        uint32_t mGeneration = 1;
        uint32_t mSeenReloadEpoch;
        std::size_t mQueuedCount = 0;
        std::vector<Slot> mSlots;
        std::vector<uint32_t> mFreeSlots;
        std::unordered_map<DedupKey, uint32_t, DedupKeyHash> mDedup;
        struct Allocation
        {
            std::weak_ptr<const void> value;
            uint64_t bytes;
        };
        std::vector<Allocation> mAllocations;
        uint64_t mDecodedBudget = 64ull * 1024 * 1024;
        uint64_t mUseSerial = 0;
    };
}
