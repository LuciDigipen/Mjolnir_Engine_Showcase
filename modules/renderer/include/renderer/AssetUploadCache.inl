// Included by AssetUploadCache.h, template lifecycle implementations.

#include <core/log/Log.h>

#include <algorithm>

namespace mir
{
    template <class Decoded, class Handle, class View, Handle View::*Member>
    const View &AssetUploadCache<Decoded, Handle, View, Member>::Request(AssetId id, const std::string &label)
    {
        const auto found = mEntries.find(id.value);
        if (found != mEntries.end())
            return found->second.mView;
        Entry &entry = mEntries[id.value];
        entry.mLabel = label;
        if (mAssets == nullptr)
            Fail(entry, "there is no asset service");
        else
        {
            entry.mRequest = mAssets->RequestLease(id, mDecoder);
            mWaiting.push_back(id.value);
        }
        return entry.mView;
    }

    template <class Decoded, class Handle, class View, Handle View::*Member>
    const View *AssetUploadCache<Decoded, Handle, View, Member>::Find(AssetId id) const
    {
        const auto found = mEntries.find(id.value);
        return found == mEntries.end() ? nullptr : &found->second.mView;
    }

    template <class Decoded, class Handle, class View, Handle View::*Member>
    void AssetUploadCache<Decoded, Handle, View, Member>::Retain(AssetId id)
    {
        auto &entry = mEntries.at(id.value);
        ++entry.mConsumers;
        entry.mLastUse = ++mUseSerial;
    }

    template <class Decoded, class Handle, class View, Handle View::*Member>
    void AssetUploadCache<Decoded, Handle, View, Member>::Drop(AssetId id)
    {
        const auto found = mEntries.find(id.value);
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
        std::erase(mWaiting, id.value);
        mEntries.erase(found);
    }

    template <class Decoded, class Handle, class View, Handle View::*Member>
    std::vector<AssetId> AssetUploadCache<Decoded, Handle, View, Member>::Update()
    {
        std::vector<AssetId> changed;
        std::erase_if(mWaiting, [&](uint64_t key)
            {
                Entry &entry = mEntries.at(key);
                const auto before = entry.mView.phase;
                if (before == AssetUploadPhase::Decoding)
                    Decode(entry);
                if (entry.mView.phase == AssetUploadPhase::Uploading)
                    Upload(entry);
                if (entry.mView.phase != before)
                    changed.push_back(AssetId{ key });
                return entry.mView.phase == AssetUploadPhase::Ready || entry.mView.phase == AssetUploadPhase::Failed;
            });
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
        const auto state = entry.mRequest.State();
        if (state == AssetLoadState::Queued)
            return;
        if (state != AssetLoadState::Ready)
        {
            Fail(entry, state == AssetLoadState::Failed ? entry.mRequest.FailureReason() :
                "its decoded data is no longer available");
            return;
        }
        const auto decoded = entry.mRequest.Acquire();
        if (!decoded)
        {
            Fail(entry, "its decoded data is missing");
            return;
        }
        const Handle handle = mQueue(decoded);
        if (handle.IsNull())
        {
            Fail(entry, "the upload could not be queued");
            return;
        }
        HandleOf(entry) = handle;
        entry.mBytes = mFootprint(handle, *decoded);
        mMetadata(entry.mView, *decoded);
        entry.mView.phase = AssetUploadPhase::Uploading;
        entry.mRequest.Reset();
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
            Fail(entry, state == UploadState::Failed ? "the GPU upload failed" : "its GPU resource was released");
        }
    }

    template <class Decoded, class Handle, class View, Handle View::*Member>
    void AssetUploadCache<Decoded, Handle, View, Member>::Trim()
    {
        uint64_t bytes = mRetiring ? mRetiring() : 0;
        std::vector<std::pair<uint64_t, uint64_t>> unused;
        for (const auto &[key, entry] : mEntries)
        {
            bytes += entry.mBytes;
            if (entry.mConsumers == 0 && entry.mView.phase == AssetUploadPhase::Ready)
                unused.emplace_back(entry.mLastUse, key);
        }
        std::sort(unused.begin(), unused.end());
        for (const auto &[serial, key] : unused)
        {
            if (bytes <= mBudget)
                break;
            auto found = mEntries.find(key);
            bytes -= found->second.mBytes;
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
    std::shared_ptr<Cache> AssetUploadRegistry<Cache>::Get(Uploader &uploader, AsyncAssetService *assets)
    {
        const uint32_t revision = assets ? assets->Revision() : 0;
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
