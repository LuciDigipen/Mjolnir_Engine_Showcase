/**
 * @file MeshLoadTracker.h
 * @author sumin.park
 * @brief Follows each mesh asset from its background decode to a usable GPU copy.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <assets/AssetId.h>
#include <assets/AsyncAssetService.h>
#include <renderer/IMeshUploader.h>
#include <renderer/Mesh.h>
#include <renderer/MeshAssetDecoder.h>
#include <renderer/components/MeshBounds.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mir
{
    enum class MeshLoadPhase : uint8_t
    {
        // Read and decoded on a worker, no GPU handle yet.
        Decoding,
        // Queued with the uploader, the handle exists but may not draw.
        Uploading,
        // Draws, and bounds are known.
        Ready,
        // Nothing will draw, the reason was logged once.
        Failed
    };

    struct MeshLoad
    {
        MeshLoadPhase phase = MeshLoadPhase::Decoding;
        // Null while Decoding and after a failed upload
        MeshHandle mesh;
        // Valid once Ready
        MeshBounds bounds;
    };

    // Main thread only. One load per mesh asset, shared by every entity that names it. A load never waits,
    // Update moves it on a step each frame as the decode and the upload finish.
    class MeshLoadTracker
    {
    public:
        // assets may be null, every load then fails at once.
        MeshLoadTracker(IMeshUploader &uploader, AsyncAssetService *assets);

        // The load for id, started the first time. label names the mesh in logs.
        const MeshLoad &Request(AssetId id, const std::string &label);

        // Moves every unfinished load on. Returns the assets whose phase changed, so the caller touches only
        // the entities that use them.
        std::vector<AssetId> Update();

        const MeshLoad *Find(AssetId id) const;
        void Retain(AssetId id);
        void Drop(AssetId id);
        void EvictUnused();
        void SetBudget(uint64_t bytes);
        bool Empty() const { return mEntries.empty(); }

        // Forgets every load without touching the uploader, for a renderer that released its own meshes.
        void Reset();

        // Retires the meshes that are queued or uploading, appends the Ready ones to ready for the caller to
        // release, then forgets every load.
        void Release(std::vector<MeshHandle> &ready);

    private:
        struct Entry
        {
            MeshLoad load;
            AssetLease<DecodedMesh> request;
            std::string label;
            std::size_t consumers = 0;
            uint64_t bytes = 0;
            uint64_t lastUse = 0;
        };

        void StepDecoding(Entry &entry);
        void StepUploading(Entry &entry);
        void Fail(Entry &entry, std::string_view reason);

        IMeshUploader &mUploader;
        AsyncAssetService *mAssets;
        std::unordered_map<uint64_t, Entry> mEntries;
        // the ids still Decoding or Uploading, so a settled scene costs nothing per frame
        std::vector<uint64_t> mWaiting;
        uint64_t mBudget = 256ull * 1024 * 1024;
        uint64_t mUseSerial = 0;
        void Trim();
    };

    // A World's claim on a shared GPU mesh. The cache outlives the claim, shutdown empties it
    // before destroying the uploader, so a later claim destructor never accesses a dead device.
    class MeshLoadLease
    {
    public:
        MeshLoadLease() = default;
        MeshLoadLease(std::shared_ptr<MeshLoadTracker> tracker, AssetId id)
            : mTracker(std::move(tracker)), mId(id) { mTracker->Retain(mId); }
        ~MeshLoadLease() { Reset(); }
        MeshLoadLease(const MeshLoadLease &) = delete;
        MeshLoadLease &operator=(const MeshLoadLease &) = delete;
        MeshLoadLease(MeshLoadLease &&other) noexcept
            : mTracker(std::move(other.mTracker)), mId(other.mId) {}
        MeshLoadLease &operator=(MeshLoadLease &&other) noexcept
        {
            if (this != &other)
            {
                Reset();
                mTracker = std::move(other.mTracker);
                mId = other.mId;
            }
            return *this;
        }
        void Reset()
        {
            if (mTracker)
            {
                mTracker->Drop(mId);
                mTracker.reset();
            }
        }
        const MeshLoad *Get() const { return mTracker ? mTracker->Find(mId) : nullptr; }

    private:
        std::shared_ptr<MeshLoadTracker> mTracker;
        AssetId mId;
    };
}
