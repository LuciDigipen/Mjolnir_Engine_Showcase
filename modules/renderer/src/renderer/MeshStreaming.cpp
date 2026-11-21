/**
 * @file MeshStreaming.cpp
 * @author Jackey Cen
 * @brief The staging ring and the frame-budgeted queue behind asynchronous mesh uploads.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer/MeshStreaming.h>

#include <algorithm>
#include <utility>

namespace mir
{
    size_t SelectBlasCompactions(std::span<const uint64_t> sourceBytes, uint64_t cap)
    {
        if (sourceBytes.empty())
            return 0;
        uint64_t used = 0;
        size_t count = 0;
        for (const uint64_t bytes : sourceBytes)
        {
            if (count > 0 && (used >= cap || bytes > cap - used))
                break;
            used += bytes;
            ++count;
        }
        return count;
    }

    MeshUploadSource MeshUploadSource::Own(std::vector<Vertex> vertices, std::vector<uint32_t> indices)
    {
        struct Payload
        {
            std::vector<Vertex> vertices;
            std::vector<uint32_t> indices;
        };

        const auto payload = std::make_shared<const Payload>(Payload{std::move(vertices), std::move(indices)});
        return MeshUploadSource{payload->vertices, payload->indices, payload};
    }

    std::optional<uint64_t> StagingRing::Allocate(uint64_t bytes, uint64_t alignment, uint64_t frame)
    {
        if (bytes == 0 || bytes > mCapacity || alignment == 0)
            return std::nullopt;
        // No copy owns an offset now; discard wrap padding before admitting the next block.
        if (mSpans.empty())
            mHead = mTail = 0;
        uint64_t start = (mHead + alignment - 1) / alignment * alignment;
        // A block never straddles the end of the ring.
        if (start % mCapacity + bytes > mCapacity)
            start = (start / mCapacity + 1) * mCapacity;
        const uint64_t end = start + bytes;
        if (end - mTail > mCapacity)
            return std::nullopt;
        mHead = end;
        mSpans.push_back({end, frame});
        return start % mCapacity;
    }

    void StagingRing::Release(uint64_t completed)
    {
        while (!mSpans.empty() && mSpans.front().frame <= completed)
        {
            mTail = mSpans.front().end;
            mSpans.pop_front();
        }
        if (mSpans.empty())
            mTail = mHead;
    }

    void StagingRing::Clear()
    {
        mSpans.clear();
        mTail = mHead;
    }

    MeshUploadQueue::MeshUploadQueue(const MeshStreamingDesc &desc)
        : mDesc(desc), mRing(desc.stagingBytes / kStagingAlignment * kStagingAlignment)
    {
    }

    void MeshUploadQueue::Push(const Item &item)
    {
        mPending.push_back(item);
        mPendingBytes += item.gpuBytes;
    }

    bool MeshUploadQueue::Cancel(MeshHandle mesh)
    {
        const auto it =
            std::find_if(mPending.begin(), mPending.end(), [&](const Item &item)
                { return item.texture.IsNull() && item.mesh == mesh; });
        if (it == mPending.end())
            return false;
        mPendingBytes -= it->gpuBytes;
        mPending.erase(it);
        return true;
    }

    std::vector<MeshUploadQueue::Admitted> MeshUploadQueue::Admit(uint64_t frame)
    {
        std::vector<Admitted> admitted;
        uint64_t used = 0;
        while (!mPending.empty())
        {
            const Item item = mPending.front();
            if (!item.texture.IsNull())
            {
                if (item.rowBytes == 0 || item.rowCount == 0 || item.rowBytes > mRing.Capacity() ||
                    (used > 0 && (used >= mDesc.frameBytes || item.rowBytes > mDesc.frameBytes - used)))
                    break;
                const uint64_t budget = used < mDesc.frameBytes ? mDesc.frameBytes - used : 0;
                uint32_t rows = static_cast<uint32_t>(std::min<uint64_t>(item.rowCount,
                    std::min(mRing.Capacity() / item.rowBytes, std::max<uint64_t>(1, budget / item.rowBytes))));
                std::optional<uint64_t> offset;
                for (;;)
                {
                    offset = mRing.Allocate(uint64_t(rows) * item.rowBytes, kStagingAlignment, frame);
                    if (offset || rows == 1)
                        break;
                    rows /= 2;
                }
                if (!offset)
                    break;
                Item band = item;
                band.rowCount = rows;
                band.stagingBytes = band.gpuBytes = uint64_t(rows) * item.rowBytes;
                mPending.pop_front();
                if (rows < item.rowCount)
                {
                    Item remainder = item;
                    remainder.firstRow += rows;
                    remainder.rowCount -= rows;
                    remainder.stagingBytes -= band.stagingBytes;
                    remainder.gpuBytes -= band.gpuBytes;
                    mPending.push_back(remainder);
                }
                mPendingBytes -= band.gpuBytes;
                mUploadingBytes += band.gpuBytes;
                used += band.gpuBytes;
                mInFlight.push_back({ band, frame });
                admitted.push_back({ band, offset });
                continue;
            }
            if (used > 0 && (used >= mDesc.frameBytes || item.gpuBytes > mDesc.frameBytes - used))
                break;
            std::optional<uint64_t> offset;
            if (item.stagingBytes <= mRing.Capacity())
            {
                offset = mRing.Allocate(item.stagingBytes, kStagingAlignment, frame);
                if (!offset)
                    break;
            }
            mPending.pop_front();
            mPendingBytes -= item.gpuBytes;
            mUploadingBytes += item.gpuBytes;
            used += item.gpuBytes;
            mInFlight.push_back({item, frame});
            admitted.push_back({item, offset});
        }
        mLastFrameBytes = used;
        return admitted;
    }

    std::vector<MeshHandle> MeshUploadQueue::Complete(uint64_t completed)
    {
        std::vector<MeshHandle> done;
        for (const Item &item : CompleteUploads(completed))
            if (item.texture.IsNull())
                done.push_back(item.mesh);
        return done;
    }

    std::vector<MeshUploadQueue::Item> MeshUploadQueue::CompleteUploads(uint64_t completed)
    {
        mRing.Release(completed);
        std::vector<Item> done;
        while (!mInFlight.empty() && mInFlight.front().frame <= completed)
        {
            done.push_back(mInFlight.front().item);
            mUploadingBytes -= mInFlight.front().item.gpuBytes;
            mInFlight.pop_front();
        }
        return done;
    }

    bool MeshUploadQueue::Cancel(TextureHandle texture)
    {
        bool cancelled = false;
        std::erase_if(mPending, [&](const Item &item)
            {
                if (item.texture.IsNull() || item.texture != texture)
                    return false;
                mPendingBytes -= item.gpuBytes;
                cancelled = true;
                return true;
            });
        return cancelled;
    }

    void MeshUploadQueue::SetFrameBytes(uint64_t frameBytes)
    {
        mDesc.frameBytes = frameBytes;
    }

    void MeshUploadQueue::Clear()
    {
        mPending.clear();
        mInFlight.clear();
        mRing.Clear();
        mPendingBytes = 0;
        mUploadingBytes = 0;
        mLastFrameBytes = 0;
    }
} // namespace mir
