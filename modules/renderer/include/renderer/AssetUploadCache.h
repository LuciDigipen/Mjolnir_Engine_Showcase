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
#include <renderer/GpuResidency.h>
#include <renderer/UploadState.h>

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mir
{
    enum class AssetUploadPhase : uint8_t { Decoding, Uploading, Ready, Failed };

    // What an upload record was made from. Content is the decoded identity from the asset service, zero for a
    // cache that lives for one catalog revision. A cache shared across revisions always passes content, so a
    // changed asset never reuses an old record while an unchanged one keeps its GPU resource.
    struct UploadKey
    {
        AssetId asset;
        uint64_t content = 0;

        UploadKey() = default;
        UploadKey(AssetId assetId, uint64_t contentId = 0) : asset(assetId), content(contentId) {}
        bool operator==(const UploadKey &) const = default;
    };

    // The content part of an UploadKey for a request to the service: the decoded content when the catalog knows it,
    // else a value qualified by the catalog revision with the top bit set, which never equals known content.
    inline uint64_t UploadContent(AsyncAssetService *assets, AssetId id, std::span<const uint32_t> tags = {})
    {
        if (assets == nullptr)
            return 0x8000000000000000ull;
        const uint64_t content = assets->ContentOfTags(id, tags);
        return content != 0 ? content : 0x8000000000000000ull | (uint64_t(assets->Revision()) + 1);
    }

    struct UploadKeyHash
    {
        std::size_t operator()(const UploadKey &key) const
        {
            return std::hash<uint64_t>{}(key.asset.value) ^ (std::hash<uint64_t>{}(key.content) * 0x9E3779B97F4A7C15ull);
        }
    };

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
            std::function<uint64_t()> retiring = {}, std::shared_ptr<GpuResidency> residency = nullptr,
            GpuCategory category = GpuCategory::Mesh)
            : mAssets(assets), mDecoder(decoder), mQueue(std::move(queue)), mState(std::move(state)),
              mRetire(std::move(retire)), mFootprint(std::move(footprint)), mMetadata(std::move(metadata)),
              mRetiring(std::move(retiring)), mResidency(std::move(residency)), mCategory(category)
        {
            if (mResidency)
            {
                mHolder = mResidency->AddHolder(mCategory, [this] { return HeldBytes(); });
                if (mRetiring)
                    mResidency->SetRetiring(mCategory, mRetiring);
            }
        }
        ~AssetUploadCache()
        {
            if (mResidency)
                mResidency->RemoveHolder(mHolder);
        }
        AssetUploadCache(const AssetUploadCache &) = delete;
        AssetUploadCache &operator=(const AssetUploadCache &) = delete;

        const View &Request(UploadKey key, const std::string &label);
        // Requests a record from data already in memory. make runs only when the record is absent, the caller
        // chooses key so it names the content and creation options. Use Request for cooked assets.
        const View &RequestInline(UploadKey key, const std::function<std::shared_ptr<const Decoded>()> &make,
            const std::string &label);
        const View *Find(UploadKey key) const;
        void Retain(UploadKey key);
        void Drop(UploadKey key);
        std::vector<UploadKey> Update();
        void Reset() { mEntries.clear(); mWaiting.clear(); }
        void Release(std::vector<Handle> &ready);
        void EvictUnused();
        // A cap for this cache alone, beside the renderer-wide residency target.
        void SetBudget(uint64_t bytes) { mCap = bytes; Trim(); }
        uint64_t HeldBytes() const;
        bool Empty() const { return mEntries.empty(); }

    private:
        struct Entry
        {
            View mView;
            AssetLease<Decoded> mRequest;
            // Data that did not come from the asset service, dropped once queued.
            std::shared_ptr<const Decoded> mInline;
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
        void Publish(Entry &entry, const std::shared_ptr<const Decoded> &decoded);
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
        std::unordered_map<UploadKey, Entry, UploadKeyHash> mEntries;
        std::vector<UploadKey> mWaiting;
        std::shared_ptr<GpuResidency> mResidency;
        GpuCategory mCategory;
        uint32_t mHolder = 0;
        std::optional<uint64_t> mCap;
        uint64_t mUseSerial = 0;
    };

    template <class Cache> class AssetUploadLease
    {
    public:
        AssetUploadLease() = default;
        AssetUploadLease(std::shared_ptr<Cache> cache, UploadKey id) : mCache(std::move(cache)), mId(id)
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
        UploadKey mId;
    };

    template <class Cache> class AssetUploadRegistry
    {
    public:
        // A cache shared across revisions keys its records by content, one per revision drops with its catalog.
        template <class Uploader>
        std::shared_ptr<Cache> Get(Uploader &uploader, AsyncAssetService *assets, bool sharedAcrossRevisions = false);
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
