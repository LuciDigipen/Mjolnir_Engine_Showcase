/**
 * @file ContentThumbnailCache.h
 * @author sumin.park
 * @brief Visibility-driven thumbnail requests and bounded GPU caching.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <assets/AsyncAssetService.h>
#include <renderer/ITextureUploader.h>

#include <atomic>
#include <filesystem>
#include <future>
#include <unordered_map>
#include <unordered_set>

namespace mir
{
    class BackgroundJobQueue;
    class JobExecutor;

    struct ContentThumbnail
    {
        TextureHandle mTexture;
        uint32_t mWidth = 0, mHeight = 0;
    };

    class ContentThumbnailCache
    {
    public:
        ContentThumbnailCache(AsyncAssetService *assets, ITextureUploader &uploader, JobExecutor *jobs = nullptr);
        ~ContentThumbnailCache();
        ContentThumbnailCache(const ContentThumbnailCache &) = delete;
        ContentThumbnailCache &operator=(const ContentThumbnailCache &) = delete;
        void BeginFrame(size_t capacity);
        ContentThumbnail Request(AssetId id, const std::filesystem::path &source = {});
        void EndFrame();
        void Clear();
        size_t Size() const { return mEntries.size(); }

    private:
        struct SourceResult
        {
            TextureUploadSource mSource;
            std::string mError;
        };
        struct Entry
        {
            uint64_t mContent = 0, mUse = 0;
            AssetLease<TextureUploadSource> mRequest;
            ContentThumbnail mPreview;
            std::shared_ptr<std::atomic<bool>> mCancelled;
            std::future<SourceResult> mSource;
        };
        void Remove(uint64_t id);
        AsyncAssetService *mAssets;
        ITextureUploader &mUploader;
        size_t mCapacity = 256;
        uint64_t mSerial = 0;
        uint32_t mRevision = 0;
        std::unordered_map<uint64_t, Entry> mEntries;
        std::unordered_map<uint64_t, uint64_t> mFailures;
        std::unordered_set<uint64_t> mVisible;
        std::unordered_map<uint64_t, std::filesystem::path> mSources;
        std::unique_ptr<BackgroundJobQueue> mSourceJobs;
    };
}
