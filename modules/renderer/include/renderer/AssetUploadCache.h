/**
 * @file AssetUploadCache.h
 * @author sumin.park
 * @brief Shared decode-to-upload lifecycle, consumer claims and unused GPU residency.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <assets/AsyncAssetService.h>
#include <renderer/UploadState.h>

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mir
{
    enum class AssetUploadPhase : uint8_t { Decoding, Uploading, Ready, Failed };

    template <class Decoded, class Handle, class View, Handle View::*Member>
    class AssetUploadCache
    {
    public:
        using ViewType = View;
        using HandleType = Handle;
        using Queue = std::function<Handle(std::shared_ptr<const Decoded>)>;
        using State = std::function<UploadState(Handle)>;
        using Retire = std::function<void(Handle)>;
        using Footprint = std::function<uint64_t(Handle, const Decoded &)>;
        using Metadata = std::function<void(View &, const Decoded &)>;
        AssetUploadCache(AsyncAssetService *assets, const AssetDecoder<Decoded> &decoder,
            Queue queue, State state, Retire retire, Footprint footprint, Metadata metadata,
            std::function<uint64_t()> retiring = {})
            : mAssets(assets), mDecoder(decoder), mQueue(std::move(queue)), mState(std::move(state)),
              mRetire(std::move(retire)), mFootprint(std::move(footprint)), mMetadata(std::move(metadata)),
              mRetiring(std::move(retiring)) {}

        const View &Request(AssetId id, const std::string &label);
        const View *Find(AssetId id) const;
        void Retain(AssetId id);
        void Drop(AssetId id);
        std::vector<AssetId> Update();
        void Reset() { mEntries.clear(); mWaiting.clear(); }
        void Release(std::vector<Handle> &ready);
        void EvictUnused();
        void SetBudget(uint64_t bytes) { mBudget = bytes; Trim(); }
        bool Empty() const { return mEntries.empty(); }

    private:
        struct Entry
        {
            View mView;
            AssetLease<Decoded> mRequest;
            std::string mLabel;
            size_t mConsumers = 0;
            uint64_t mBytes = 0;
            uint64_t mLastUse = 0;
        };
        static Handle &HandleOf(Entry &entry) { return entry.mView.*Member; }
        static const Handle &HandleOf(const Entry &entry) { return entry.mView.*Member; }
        void RetireEntry(const Entry &entry);
        void Fail(Entry &entry, const std::string &reason);
        void Decode(Entry &entry);
        void Upload(Entry &entry);
        void Trim();
        AsyncAssetService *mAssets;
        const AssetDecoder<Decoded> &mDecoder;
        Queue mQueue;
        State mState;
        Retire mRetire;
        Footprint mFootprint;
        Metadata mMetadata;
        std::function<uint64_t()> mRetiring;
        std::unordered_map<uint64_t, Entry> mEntries;
        std::vector<uint64_t> mWaiting;
        uint64_t mBudget = 256ull * 1024 * 1024;
        uint64_t mUseSerial = 0;
    };

    template <class Cache> class AssetUploadLease
    {
    public:
        AssetUploadLease() = default;
        AssetUploadLease(std::shared_ptr<Cache> cache, AssetId id) : mCache(std::move(cache)), mId(id)
            { mCache->Retain(mId); }
        ~AssetUploadLease() { Reset(); }
        AssetUploadLease(const AssetUploadLease &) = delete;
        AssetUploadLease &operator=(const AssetUploadLease &) = delete;
        AssetUploadLease(AssetUploadLease &&other) noexcept : mCache(std::move(other.mCache)), mId(other.mId) {}
        AssetUploadLease &operator=(AssetUploadLease &&other) noexcept;
        void Reset();
        const typename Cache::ViewType *Get() const { return mCache ? mCache->Find(mId) : nullptr; }
    private:
        std::shared_ptr<Cache> mCache;
        AssetId mId;
    };

    template <class Cache> class AssetUploadRegistry
    {
    public:
        template <class Uploader> std::shared_ptr<Cache> Get(Uploader &uploader, AsyncAssetService *assets);
        template <class Retire> void Release(Retire retire);
    private:
        struct Entry
        {
            AsyncAssetService *mAssets;
            uint32_t mRevision;
            std::shared_ptr<Cache> mCache;
        };
        std::vector<Entry> mCaches;
    };
}

// Template definitions must remain visible to callers.
#include "renderer/AssetUploadCache.inl"
