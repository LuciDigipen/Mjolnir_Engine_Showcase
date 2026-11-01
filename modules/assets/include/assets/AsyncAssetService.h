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

#include <cstddef>
#include <cstdint>
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

    // Where a request is. Stale means it predates CancelAll or a reload, or was never made.
    enum class AssetLoadState : uint8_t
    {
        Stale,
        Queued,
        Ready,
        Failed
    };

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
            return RequestErased(id, &decoder, typeid(T), decoder.dependencyTags,
                [decode = decoder.decode](const AssetBlobView &root, const AssetDependencies &dependencies,
                    std::string &error) -> std::shared_ptr<const void>
                {
                    auto value = std::make_shared<T>();
                    if (!decode(root, dependencies, *value, error))
                        return nullptr;
                    return value;
                });
        }

        AssetLoadState State(AssetRequest request) const;

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

        // Drops every request and result, cancels loads that have not started and waits for the
        // ones running. Every earlier handle reports Stale afterwards.
        void CancelAll();

        // True when no request is waiting for a result
        bool Idle() const;

    private:
        using ErasedDecode = std::function<std::shared_ptr<const void>(
            const AssetBlobView &root, const AssetDependencies &dependencies, std::string &error)>;

        struct Completions;
        struct Job;

        struct Slot
        {
            Slot(AssetId assetId, std::type_index assetType) : id(assetId), type(assetType) {}

            AssetId id;
            std::type_index type;
            AssetLoadState state = AssetLoadState::Queued;
            std::shared_ptr<const void> result;
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
            std::span<const uint32_t> dependencyTags, ErasedDecode decode);
        std::shared_ptr<const void> AcquireErased(AssetRequest request, std::type_index type) const;
        static void RunJob(Job &job) noexcept;
        // CancelAll when a reload was committed since the last call
        void SyncReloadEpoch();

        AssetsModule &mAssets;
        JobExecutor &mJobs;
        std::size_t mConcurrency;
        std::unique_ptr<BackgroundJobQueue> mQueue;
        std::shared_ptr<Completions> mCompletions;

        // starts at 1 so a default AssetRequest is Stale
        uint32_t mGeneration = 1;
        uint32_t mSeenReloadEpoch;
        std::size_t mQueuedCount = 0;
        std::vector<Slot> mSlots;
        std::unordered_map<DedupKey, uint32_t, DedupKeyHash> mDedup;
    };
}
