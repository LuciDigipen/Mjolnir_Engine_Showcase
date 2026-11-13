// Included by AssetUploadCache.h, template lifecycle implementations.

#include <core/log/Log.h>

#include <algorithm>

namespace mir
{
    template <class Decoded, class Handle, class View, Handle View::*Member>
    const View &AssetUploadCache<Decoded, Handle, View, Member>::Request(UploadKey key, const std::string &label)
    {
        const auto found = mEntries.find(key);
        if (found != mEntries.end())
        {
            // A failure is remembered while the catalog stays the same, and a reload allows another attempt.
            if (Retry(found->second))
            {
                found->second.mRequest = mAssets->RequestLease(key.asset, mDecoder);
                mWaiting.push_back(key);
            }
            return found->second.mView;
        }
        Entry &entry = mEntries[key];
        entry.mKey = key;
        entry.mLabel = label;
        entry.mAttempt = Attempt();
        if (mAssets == nullptr)
            Fail(entry, "there is no asset service");
        else
        {
            entry.mRequest = mAssets->RequestLease(key.asset, mDecoder);
            mWaiting.push_back(key);
        }
        return entry.mView;
    }

    template <class Decoded, class Handle, class View, Handle View::*Member>
    const View &AssetUploadCache<Decoded, Handle, View, Member>::RequestInline(UploadKey key,
        const std::function<std::shared_ptr<const Decoded>()> &make, const std::string &label)
    {
        const auto found = mEntries.find(key);
        if (found != mEntries.end())
        {
            if (Retry(found->second))
            {
                found->second.mInline = make();
                if (found->second.mInline)
                    mWaiting.push_back(key);
                else
                    Fail(found->second, "there is no data to upload");
            }
            return found->second.mView;
        }
        Entry &entry = mEntries[key];
        entry.mKey = key;
        entry.mLabel = label;
        entry.mAttempt = Attempt();
        entry.mInline = make();
        if (!entry.mInline)
            Fail(entry, "there is no data to upload");
        else
            mWaiting.push_back(key);
        return entry.mView;
    }

    template <class Decoded, class Handle, class View, Handle View::*Member>
    bool AssetUploadCache<Decoded, Handle, View, Member>::Retry(Entry &entry)
    {
        if (entry.mView.phase != AssetUploadPhase::Failed || mAssets == nullptr || entry.mAttempt == Attempt())
            return false;
        // The failed attempt held nothing. The claims on the record stay valid and see the new attempt.
        entry.mAttempt = Attempt();
        entry.mView.phase = AssetUploadPhase::Decoding;
        HandleOf(entry) = Handle{};
        entry.mBytes = 0;
        entry.mInline.reset();
        return true;
    }

    template <class Decoded, class Handle, class View, Handle View::*Member>
    uint64_t AssetUploadCache<Decoded, Handle, View, Member>::HeldBytes() const
    {
        uint64_t bytes = 0;
        for (const auto &[key, entry] : mEntries)
            bytes += entry.mBytes;
        return bytes;
    }

    template <class Decoded, class Handle, class View, Handle View::*Member>
    uint64_t AssetUploadCache<Decoded, Handle, View, Member>::BytesOf(UploadKey key) const
    {
        const auto found = mEntries.find(key);
        return found == mEntries.end() ? 0 : found->second.mBytes;
    }

    template <class Decoded, class Handle, class View, Handle View::*Member>
    size_t AssetUploadCache<Decoded, Handle, View, Member>::ConsumersOf(UploadKey key) const
    {
        const auto found = mEntries.find(key);
        return found == mEntries.end() ? 0 : found->second.mConsumers;
    }

    template <class Decoded, class Handle, class View, Handle View::*Member>
    const View *AssetUploadCache<Decoded, Handle, View, Member>::Find(UploadKey key) const
    {
        const auto found = mEntries.find(key);
        return found == mEntries.end() ? nullptr : &found->second.mView;
    }

    template <class Decoded, class Handle, class View, Handle View::*Member>
    void AssetUploadCache<Decoded, Handle, View, Member>::Retain(UploadKey key)
    {
        auto &entry = mEntries.at(key);
        ++entry.mConsumers;
        entry.mLastUse = ++mUseSerial;
    }

    template <class Decoded, class Handle, class View, Handle View::*Member>
    void AssetUploadCache<Decoded, Handle, View, Member>::Drop(UploadKey key)
    {
        const auto found = mEntries.find(key);
        if (found == mEntries.end() || found->second.mConsumers == 0)
            return;
        Entry &entry = found->second;
        if (--entry.mConsumers != 0)
            return;
        if (entry.mView.phase == AssetUploadPhase::Ready)
        {
            entry.mLastUse = ++mUseSerial;
            Trim();
            return;
        }
        RetireEntry(entry);
        std::erase(mWaiting, key);
        mEntries.erase(found);
    }

    template <class Decoded, class Handle, class View, Handle View::*Member>
    std::vector<UploadKey> AssetUploadCache<Decoded, Handle, View, Member>::Update()
    {
        std::vector<UploadKey> changed;
        std::erase_if(mWaiting, [&](const UploadKey &key)
            {
                Entry &entry = mEntries.at(key);
                const auto before = entry.mView.phase;
                if (before == AssetUploadPhase::Decoding)
                    Decode(entry);
                if (entry.mView.phase == AssetUploadPhase::Uploading)
                    Upload(entry);
                if (entry.mView.phase != before)
                    changed.push_back(key);
                return entry.mView.phase == AssetUploadPhase::Ready || entry.mView.phase == AssetUploadPhase::Failed;
            });
        // New allocations can put the cache over its target while no lease is released, so trim here as well.
        if (!changed.empty())
        {
            Trim();
            std::erase_if(changed, [this](const UploadKey &key) { return !mEntries.contains(key); });
        }
        return changed;
    }

    template <class Decoded, class Handle, class View, Handle View::*Member>
    void AssetUploadCache<Decoded, Handle, View, Member>::Release(std::vector<Handle> &ready)
    {
        for (auto &[key, entry] : mEntries)
            if (!HandleOf(entry).IsNull())
            {
                if (entry.mView.phase == AssetUploadPhase::Ready)
                    ready.push_back(HandleOf(entry));
                else
                    RetireEntry(entry);
            }
        Reset();
    }

    template <class Decoded, class Handle, class View, Handle View::*Member>
    void AssetUploadCache<Decoded, Handle, View, Member>::EvictUnused()
    {
        std::erase_if(mEntries, [&](auto &item)
            {
                if (item.second.mConsumers != 0)
                    return false;
                RetireEntry(item.second);
                std::erase(mWaiting, item.first);
                return true;
            });
    }

    template <class Decoded, class Handle, class View, Handle View::*Member>
    void AssetUploadCache<Decoded, Handle, View, Member>::RetireEntry(const Entry &entry)
    {
        if (!HandleOf(entry).IsNull())
            mRetire(HandleOf(entry));
    }

    template <class Decoded, class Handle, class View, Handle View::*Member>
    void AssetUploadCache<Decoded, Handle, View, Member>::Fail(Entry &entry, const std::string &reason)
    {
        MIR_LOG_ERROR("AssetUploadCache: asset '{}' will not draw: {}", entry.mLabel, reason);
        entry.mView.phase = AssetUploadPhase::Failed;
    }

    template <class Decoded, class Handle, class View, Handle View::*Member>
    void AssetUploadCache<Decoded, Handle, View, Member>::Decode(Entry &entry)
    {
        if (entry.mInline)
        {
            if (mAdmit && !mAdmit())
                return;
            const auto decoded = std::move(entry.mInline);
            entry.mInline.reset();
            Publish(entry, decoded);
            return;
        }
        const auto state = entry.mRequest.State();
        if (state == AssetLoadState::Queued)
            return;
        if (state != AssetLoadState::Ready)
        {
            Fail(entry, state == AssetLoadState::Failed ? entry.mRequest.FailureReason() :
                "its decoded data is no longer available");
            return;
        }
        if (mAdmit && !mAdmit())
            return;
        const auto decoded = entry.mRequest.Acquire();
        if (!decoded)
        {
            Fail(entry, "its decoded data is missing");
            return;
        }
        Publish(entry, decoded);
        entry.mRequest.Reset();
    }

    template <class Decoded, class Handle, class View, Handle View::*Member>
    void AssetUploadCache<Decoded, Handle, View, Member>::Publish(Entry &entry,
        const std::shared_ptr<const Decoded> &decoded)
    {
        const Handle handle = mQueue(entry.mKey, decoded);
        if (handle.IsNull())
        {
            Fail(entry, "the upload could not be queued");
            return;
        }
        HandleOf(entry) = handle;
        entry.mBytes = mFootprint(entry.mKey, handle, *decoded);
        mMetadata(entry.mView, *decoded);
        entry.mView.phase = AssetUploadPhase::Uploading;
    }

    template <class Decoded, class Handle, class View, Handle View::*Member>
    void AssetUploadCache<Decoded, Handle, View, Member>::Upload(Entry &entry)
    {
        const auto state = mState(HandleOf(entry));
        if (state == UploadState::Pending || state == UploadState::Uploading)
            return;
        if (state == UploadState::Ready)
            entry.mView.phase = AssetUploadPhase::Ready;
        else
        {
            if (state == UploadState::Failed)
                RetireEntry(entry);
            HandleOf(entry) = Handle{};
            // the resource is gone, its footprint must not count toward the target
            entry.mBytes = 0;
            Fail(entry, state == UploadState::Failed ? "the GPU upload failed" : "its GPU resource was released");
        }
    }

    template <class Decoded, class Handle, class View, Handle View::*Member>
    void AssetUploadCache<Decoded, Handle, View, Member>::Trim()
    {
        uint64_t need = mResidency ? mResidency->Excess(mCategory) : 0;
        if (mCap)
        {
            const uint64_t held = HeldBytes();
            if (held > *mCap)
                need = std::max(need, held - *mCap);
        }
        if (need == 0)
            return;
        std::vector<std::pair<uint64_t, UploadKey>> unused;
        for (const auto &[key, entry] : mEntries)
            if (entry.mConsumers == 0 && entry.mView.phase == AssetUploadPhase::Ready)
                unused.emplace_back(entry.mLastUse, key);
        std::sort(unused.begin(), unused.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
        uint64_t freed = 0;
        for (const auto &[serial, key] : unused)
        {
            if (freed >= need)
                break;
            auto found = mEntries.find(key);
            freed += found->second.mBytes;
            RetireEntry(found->second);
            mEntries.erase(found);
        }
    }

    template <class Cache>
    AssetUploadLease<Cache> &AssetUploadLease<Cache>::operator=(AssetUploadLease &&other) noexcept
    {
        if (this != &other)
        {
            Reset();
            mCache = std::move(other.mCache);
            mId = other.mId;
        }
        return *this;
    }

    template <class Cache>
    void AssetUploadLease<Cache>::Reset()
    {
        if (mCache)
        {
            mCache->Drop(mId);
            mCache.reset();
        }
    }

    template <class Cache>
    template <class Uploader>
    std::shared_ptr<Cache> AssetUploadRegistry<Cache>::Get(Uploader &uploader, AsyncAssetService *assets,
        bool sharedAcrossRevisions)
    {
        const uint32_t revision = assets && !sharedAcrossRevisions ? assets->Revision() : 0;
        std::erase_if(mCaches, [assets, revision](const Entry &cache)
            {
                if (cache.mAssets != assets || cache.mRevision == revision || cache.mCache.use_count() != 1)
                    return false;
                cache.mCache->EvictUnused();
                return cache.mCache->Empty();
            });
        for (const auto &entry : mCaches)
            if (entry.mAssets == assets && entry.mRevision == revision)
                return entry.mCache;
        auto cache = std::make_shared<Cache>(uploader, assets);
        mCaches.push_back({ assets, revision, cache });
        return cache;
    }

    template <class Cache>
    template <class Retire>
    void AssetUploadRegistry<Cache>::Release(Retire retire)
    {
        for (auto &entry : mCaches)
        {
            std::vector<typename Cache::HandleType> ready;
            entry.mCache->Release(ready);
            for (const auto handle : ready)
                retire(handle);
        }
        mCaches.clear();
    }
}
