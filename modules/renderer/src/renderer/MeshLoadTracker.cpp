/**
 * @file MeshLoadTracker.cpp
 * @author sumin.park
 * @brief Follows each mesh asset from its background decode to a usable GPU copy.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer/MeshLoadTracker.h>

#include <core/log/Log.h>
#include <renderer/MeshAssetDecoder.h>
#include <renderer/MeshStreaming.h>

#include <algorithm>
#include <memory>
#include <utility>

namespace mir
{
    MeshLoadTracker::MeshLoadTracker(IMeshUploader &uploader, AsyncAssetService *assets)
        : mUploader(uploader), mAssets(assets)
    {}

    const MeshLoad &MeshLoadTracker::Request(AssetId id, const std::string &label)
    {
        const auto found = mEntries.find(id.value);
        if (found != mEntries.end())
            return found->second.load;

        Entry &entry = mEntries[id.value];
        entry.label = label;
        if (mAssets == nullptr)
        {
            Fail(entry, "there is no asset service");
            return entry.load;
        }

        entry.request = mAssets->RequestLease(id, kMeshAssetDecoder);
        mWaiting.push_back(id.value);
        return entry.load;
    }

    const MeshLoad *MeshLoadTracker::Find(AssetId id) const
    {
        const auto found = mEntries.find(id.value);
        return found != mEntries.end() ? &found->second.load : nullptr;
    }

    void MeshLoadTracker::Retain(AssetId id)
    {
        ++mEntries.at(id.value).consumers;
        mEntries.at(id.value).lastUse = ++mUseSerial;
    }

    void MeshLoadTracker::Drop(AssetId id)
    {
        const auto found = mEntries.find(id.value);
        if (found == mEntries.end() || found->second.consumers == 0)
            return;
        Entry &entry = found->second;
        --entry.consumers;
        if (entry.consumers != 0)
            return;
        if (entry.load.phase == MeshLoadPhase::Ready)
        {
            entry.lastUse = ++mUseSerial;
            Trim();
            return;
        }
        if (!entry.load.mesh.IsNull())
            mUploader.RetireMesh(entry.load.mesh);
        std::erase(mWaiting, id.value);
        mEntries.erase(found);
    }

    void MeshLoadTracker::EvictUnused()
    {
        std::erase_if(mEntries, [this](const auto &item)
            {
                const Entry &entry = item.second;
                if (entry.consumers != 0)
                    return false;
                if (!entry.load.mesh.IsNull())
                    mUploader.RetireMesh(entry.load.mesh);
                std::erase(mWaiting, item.first);
                return true;
            });
    }

    void MeshLoadTracker::SetBudget(uint64_t bytes)
    {
        mBudget = bytes;
        Trim();
    }

    void MeshLoadTracker::Trim()
    {
        uint64_t bytes = mUploader.MeshStreaming().retiringBytes;
        std::vector<std::pair<uint64_t, uint64_t>> unused;
        for (const auto &[key, entry] : mEntries)
        {
            bytes += entry.bytes;
            if (entry.consumers == 0 && entry.load.phase == MeshLoadPhase::Ready)
                unused.emplace_back(entry.lastUse, key);
        }
        std::sort(unused.begin(), unused.end());
        for (const auto &[serial, key] : unused)
        {
            if (bytes <= mBudget)
                break;
            auto found = mEntries.find(key);
            bytes -= found->second.bytes;
            mUploader.RetireMesh(found->second.load.mesh);
            mEntries.erase(found);
        }
    }

    std::vector<AssetId> MeshLoadTracker::Update()
    {
        std::vector<AssetId> changed;
        std::erase_if(mWaiting,
            [&](uint64_t key)
            {
                Entry &entry = mEntries.at(key);
                const MeshLoadPhase before = entry.load.phase;

                // a decode that finishes this frame is queued for upload in the same step
                if (entry.load.phase == MeshLoadPhase::Decoding)
                    StepDecoding(entry);
                if (entry.load.phase == MeshLoadPhase::Uploading)
                    StepUploading(entry);

                if (entry.load.phase != before)
                    changed.push_back(AssetId{ key });
                return entry.load.phase == MeshLoadPhase::Ready || entry.load.phase == MeshLoadPhase::Failed;
            });
        return changed;
    }

    void MeshLoadTracker::StepDecoding(Entry &entry)
    {
        switch (entry.request.State())
        {
        case AssetLoadState::Queued:
            return;
        case AssetLoadState::Failed:
            // the service logs only the id, so the label goes out with its reason
            Fail(entry, entry.request.FailureReason());
            return;
        case AssetLoadState::Stale:
            Fail(entry, "its load was dropped by a reload");
            return;
        case AssetLoadState::Ready:
            break;
        }

        const std::shared_ptr<const DecodedMesh> decoded = entry.request.Acquire();
        if (!decoded)
        {
            Fail(entry, "its decoded mesh is missing");
            return;
        }

        // the scene's meshes still get a BLAS when the device supports ray tracing
        const MeshHandle handle = mUploader.QueueMesh(
            MeshUploadSource{ decoded->vertices, decoded->indices, decoded }, MeshUploadOptions{ .rayTracing = true });
        if (handle.IsNull())
        {
            Fail(entry, "the upload could not be queued");
            return;
        }

        entry.load.mesh = handle;
        entry.bytes = mUploader.MeshFootprint(static_cast<uint32_t>(decoded->vertices.size()),
            static_cast<uint32_t>(decoded->indices.size()), MeshUploadOptions{ .rayTracing = true });
        entry.load.bounds = decoded->bounds;
        entry.load.phase = MeshLoadPhase::Uploading;
        entry.request.Reset();
    }

    void MeshLoadTracker::StepUploading(Entry &entry)
    {
        switch (mUploader.MeshState(entry.load.mesh))
        {
        case MeshUploadState::Pending:
        case MeshUploadState::Uploading:
            return;
        case MeshUploadState::Ready:
            entry.load.phase = MeshLoadPhase::Ready;
            return;
        case MeshUploadState::Failed:
            mUploader.RetireMesh(entry.load.mesh);
            entry.load.mesh = MeshHandle{};
            Fail(entry, "the GPU upload failed");
            return;
        case MeshUploadState::Stale:
            entry.load.mesh = MeshHandle{};
            Fail(entry, "its GPU mesh was released");
            return;
        }
    }

    void MeshLoadTracker::Fail(Entry &entry, std::string_view reason)
    {
        MIR_LOG_ERROR("MeshLoadTracker: mesh '{}' will not draw: {}", entry.label, reason);
        entry.load.phase = MeshLoadPhase::Failed;
    }

    void MeshLoadTracker::Reset()
    {
        mEntries.clear();
        mWaiting.clear();
    }

    void MeshLoadTracker::Release(std::vector<MeshHandle> &ready)
    {
        for (const auto &[key, entry] : mEntries)
        {
            if (entry.load.mesh.IsNull())
                continue;
            if (entry.load.phase == MeshLoadPhase::Ready)
                ready.push_back(entry.load.mesh);
            else
                mUploader.RetireMesh(entry.load.mesh);
        }
        Reset();
    }
}
