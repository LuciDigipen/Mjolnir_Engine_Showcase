/**
 * @file FakeTextureUploader.h
 * @author sumin.park
 * @brief Controlled texture transfer completion and ownership for resource tests.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <renderer/ITextureUploader.h>
#include <renderer/TextureTable.h>

#include <unordered_map>
#include <vector>

namespace mir::test
{
    class FakeTextureUploader final : public ITextureUploader
    {
    public:
        struct Entry
        {
            TextureUploadState mState = TextureUploadState::Pending;
            TextureUploadSource mSource;
            uint64_t mBytes = 0;
        };
        TextureHandle QueueTexture(TextureUploadSource source) override
        {
            if (mReject || !EnsureTextureSlot())
                return {};
            const TextureHandle handle{ mNext++, 1 };
            const uint64_t bytes = source.Bytes();
            mEntries.emplace(handle.mIndex, Entry{ TextureUploadState::Pending, std::move(source), bytes });
            ++mQueued;
            return handle;
        }
        std::size_t TextureSlotsAvailable() const override
        {
            return mEntries.size() >= mSlotCapacity ? 0 : mSlotCapacity - mEntries.size();
        }
        TextureUploadState TextureState(TextureHandle handle) const override
        {
            const auto found = mEntries.find(handle.mIndex);
            return handle.mGeneration == 1 && found != mEntries.end() ? found->second.mState : TextureUploadState::Stale;
        }
        bool RetireTexture(TextureHandle handle) override
        {
            if (TextureState(handle) == TextureUploadState::Stale)
                return false;
            // retired memory stays occupied until the frame that last used it completes
            if (mEntries.at(handle.mIndex).mState != TextureUploadState::Pending)
                mDeferred.push_back(mEntries.at(handle.mIndex).mBytes);
            mEntries.erase(handle.mIndex);
            mRetired.push_back(handle);
            return true;
        }
        uint64_t TextureFootprint(TextureHandle handle) const override
        {
            return TextureState(handle) == TextureUploadState::Stale ? 0 : mEntries.at(handle.mIndex).mBytes;
        }
        uint64_t TextureRetiringBytes() const override
        {
            uint64_t bytes = 0;
            for (const uint64_t deferred : mDeferred)
                bytes += deferred;
            return bytes;
        }
        void CompleteFrame() { mDeferred.clear(); }
        void Begin(TextureHandle handle) { mEntries.at(handle.mIndex).mState = TextureUploadState::Uploading; }
        void CopyAll(TextureHandle handle) { mEntries.at(handle.mIndex).mSource = {}; }
        void Complete(TextureHandle handle) { mEntries.at(handle.mIndex).mState = TextureUploadState::Ready; }
        void Fail(TextureHandle handle) { mEntries.at(handle.mIndex).mState = TextureUploadState::Failed; }

        std::size_t mSlotCapacity = TextureTable::kDefaultCapacity;
        bool mReject = false;
        uint64_t mQueued = 0;
        std::unordered_map<uint32_t, Entry> mEntries;
        std::vector<TextureHandle> mRetired;
        std::vector<uint64_t> mDeferred;
    private:
        uint32_t mNext = 0;
    };
}
