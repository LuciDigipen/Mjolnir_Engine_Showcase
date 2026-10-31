/**
 * @file MaterialRecordTable.cpp
 * @author Jackey Cen
 * @brief Aligned raw-byte material records with frame-safe snapshots.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer/MaterialRecordTable.h>

#include <algorithm>
#include <cstring>

namespace mir
{
    namespace
    {
        uint32_t AlignUp(uint64_t value, uint32_t alignment)
        {
            const uint64_t aligned = (value + alignment - 1) / alignment * alignment;
            return aligned <= UINT32_MAX ? static_cast<uint32_t>(aligned) : kInvalidMaterialRecordOffset;
        }
    }

    MaterialRecordTable::MaterialRecordTable(const MaterialRecordTableDesc &desc)
        : mCapacityBytes(desc.capacityBytes),
        mAlignment(std::max(desc.alignment, 1u)),
        mFramesInFlight(std::max(desc.framesInFlight, 1u)),
        mStorage(desc.capacityBytes)
    {
        if (mCapacityBytes > 0)
            mFreeRanges.push_back(FreeSpan{ .mOffset = 0, .mSize = mCapacityBytes });
    }

    MaterialRecordTable::Block *MaterialRecordTable::FindBlock(MaterialRecordId id)
    {
        if (id.IsNull() || id.mIndex >= mBlocks.size())
            return nullptr;
        Block &block = mBlocks[id.mIndex];
        return block.mAlive && block.mGeneration == id.mGeneration ? &block : nullptr;
    }

    const MaterialRecordTable::Block *MaterialRecordTable::FindBlock(MaterialRecordId id) const
    {
        if (id.IsNull() || id.mIndex >= mBlocks.size())
            return nullptr;
        const Block &block = mBlocks[id.mIndex];
        return block.mAlive && block.mGeneration == id.mGeneration ? &block : nullptr;
    }

    uint32_t MaterialRecordTable::CopyOffset(const Block &block, uint32_t copy) const
    {
        return block.mOffset + copy * block.mStride;
    }

    bool MaterialRecordTable::AllocateRange(uint32_t bytes, uint32_t alignment, uint32_t &outOffset)
    {
        if (bytes == 0 || alignment == 0)
            return false;

        for (std::size_t i = 0; i < mFreeRanges.size(); ++i)
        {
            FreeSpan &range = mFreeRanges[i];
            const uint64_t aligned = AlignUp(range.mOffset, alignment);
            if (aligned > UINT32_MAX)
                continue;

            const uint64_t end = aligned + bytes;
            if (end > static_cast<uint64_t>(range.mOffset) + range.mSize)
                continue;

            const uint32_t prefix = static_cast<uint32_t>(aligned) - range.mOffset;
            const uint32_t suffix = range.mSize - prefix - bytes;

            if (prefix > 0 && suffix > 0)
            {
                range.mSize = prefix;
                mFreeRanges.insert(mFreeRanges.begin() + static_cast<std::ptrdiff_t>(i + 1),
                    FreeSpan{ .mOffset = static_cast<uint32_t>(end), .mSize = suffix });
            }
            else if (prefix > 0)
                range.mSize = prefix;
            else if (suffix > 0)
            {
                range.mOffset = static_cast<uint32_t>(end);
                range.mSize = suffix;
            }
            else
                mFreeRanges.erase(mFreeRanges.begin() + static_cast<std::ptrdiff_t>(i));

            outOffset = static_cast<uint32_t>(aligned);
            return true;
        }
        return false;
    }

    void MaterialRecordTable::FreeRange(uint32_t offset, uint32_t size)
    {
        if (size == 0)
            return;

        const auto it = std::lower_bound(mFreeRanges.begin(), mFreeRanges.end(), offset,
            [](const FreeSpan &range, uint32_t value) { return range.mOffset < value; });
        mFreeRanges.insert(it, FreeSpan{ .mOffset = offset, .mSize = size });

        // Merge neighbours so a reclaimed block can hold the next record whole.
        for (std::size_t i = 0; i + 1 < mFreeRanges.size();)
        {
            FreeSpan &left = mFreeRanges[i];
            FreeSpan &right = mFreeRanges[i + 1];
            if (left.mOffset + left.mSize == right.mOffset)
            {
                left.mSize += right.mSize;
                mFreeRanges.erase(mFreeRanges.begin() + static_cast<std::ptrdiff_t>(i + 1));
            }
            else
                ++i;
        }
    }

    MaterialRecordId MaterialRecordTable::Create(std::span<const std::byte> bytes, uint32_t alignment)
    {
        const uint32_t requestedAlignment = std::max(alignment == 0 ? mAlignment : alignment, mAlignment);
        const uint32_t size = static_cast<uint32_t>(std::max<std::size_t>(bytes.size(), 1));
        const uint32_t stride = AlignUp(size, requestedAlignment);
        if (stride == kInvalidMaterialRecordOffset)
            return kNullMaterialRecord;

        const uint64_t blockBytes = static_cast<uint64_t>(stride) * kCopies;
        if (blockBytes > UINT32_MAX)
            return kNullMaterialRecord;

        uint32_t offset = 0;
        if (!AllocateRange(static_cast<uint32_t>(blockBytes), requestedAlignment, offset))
            return kNullMaterialRecord;

        std::fill_n(mStorage.begin() + offset, static_cast<std::size_t>(blockBytes), std::byte{});

        uint32_t index = UINT32_MAX;
        if (!mFreeBlocks.empty())
        {
            index = mFreeBlocks.back();
            mFreeBlocks.pop_back();
        }
        else
        {
            index = static_cast<uint32_t>(mBlocks.size());
            mBlocks.push_back(Block{});
        }

        Block &block = mBlocks[index];
        const uint32_t nextGeneration = block.mGeneration + 1;
        block = Block{};
        block.mPendingBytes.resize(size);
        block.mOffset = offset;
        block.mStride = stride;
        block.mSize = size;
        block.mGeneration = nextGeneration;
        block.mAlive = true;

        if (!bytes.empty())
            std::memcpy(mStorage.data() + CopyOffset(block, 0), bytes.data(), bytes.size());

        const MaterialRecordId id{ index, block.mGeneration };
        mWrites.push_back(Write{ .mOffset = CopyOffset(block, 0),
            .mBytes = std::span<const std::byte>(mStorage).subspan(CopyOffset(block, 0), block.mSize) });
        mOffsetChanges.push_back(OffsetChange{ .mId = id, .mOffset = CopyOffset(block, 0) });
        return id;
    }

    bool MaterialRecordTable::Update(MaterialRecordId id, std::span<const std::byte> bytes)
    {
        Block *const block = FindBlock(id);
        if (block == nullptr || bytes.size() != block->mSize)
            return false;

        std::memcpy(block->mPendingBytes.data(), bytes.data(), bytes.size());
        block->mPending = true;
        return true;
    }

    bool MaterialRecordTable::Release(MaterialRecordId id)
    {
        Block *const block = FindBlock(id);
        if (block == nullptr)
            return false;

        block->mAlive = false;
        block->mPending = false;
        ++block->mGeneration;
        block->mReclaimPending = true;
        block->mReclaimFrame = mFrame + mFramesInFlight;
        return true;
    }

    bool MaterialRecordTable::Publish(uint64_t completedFrame, uint64_t frame)
    {
        mCompletedFrame = std::max(mCompletedFrame, completedFrame);
        mFrame = std::max(mFrame, frame);
        Reclaim(mCompletedFrame);

        mWrites.clear();
        mOffsetChanges.clear();

        bool published = false;
        for (std::size_t i = 0; i < mBlocks.size(); ++i)
        {
            Block &block = mBlocks[i];
            if (!block.mAlive || !block.mPending)
                continue;

            uint32_t target = UINT32_MAX;
            for (uint32_t copy = 0; copy < kCopies; ++copy)
            {
                if (copy != block.mActiveCopy && block.mCopyFreeAfter[copy] <= mCompletedFrame)
                {
                    target = copy;
                    break;
                }
            }
            if (target == UINT32_MAX)
                continue;

            const uint32_t oldCopy = block.mActiveCopy;
            std::memcpy(mStorage.data() + CopyOffset(block, target), block.mPendingBytes.data(), block.mSize);
            block.mActiveCopy = target;
            block.mPending = false;
            block.mCopyFreeAfter[oldCopy] = frame;
            block.mCopyFreeAfter[target] = 0;

            const MaterialRecordId id{ static_cast<uint32_t>(i), block.mGeneration };
            const uint32_t offset = CopyOffset(block, block.mActiveCopy);
            mWrites.push_back(Write{ .mOffset = offset,
                .mBytes = std::span<const std::byte>(mStorage).subspan(offset, block.mSize) });
            mOffsetChanges.push_back(OffsetChange{ .mId = id, .mOffset = offset });
            published = true;
        }
        return published;
    }

    void MaterialRecordTable::Reclaim(uint64_t completedFrame)
    {
        for (std::size_t i = 0; i < mBlocks.size(); ++i)
        {
            Block &block = mBlocks[i];
            if (block.mAlive || !block.mReclaimPending || block.mReclaimFrame > completedFrame)
                continue;

            FreeRange(block.mOffset, block.mStride * kCopies);
            block.mReclaimPending = false;
            mFreeBlocks.push_back(static_cast<uint32_t>(i));
        }
    }

    uint32_t MaterialRecordTable::Offset(MaterialRecordId id) const
    {
        const Block *const block = FindBlock(id);
        return block != nullptr ? CopyOffset(*block, block->mActiveCopy) : kInvalidMaterialRecordOffset;
    }

    uint32_t MaterialRecordTable::Size(MaterialRecordId id) const
    {
        const Block *const block = FindBlock(id);
        return block != nullptr ? block->mSize : 0;
    }

    bool MaterialRecordTable::Valid(MaterialRecordId id) const
    {
        return FindBlock(id) != nullptr;
    }

    std::span<const std::byte> MaterialRecordTable::PublishedBytes(MaterialRecordId id) const
    {
        const Block *const block = FindBlock(id);
        if (block == nullptr)
            return {};
        return std::span<const std::byte>(mStorage).subspan(CopyOffset(*block, block->mActiveCopy), block->mSize);
    }

    uint32_t MaterialRecordTable::UsedBytes() const
    {
        uint32_t freeBytes = 0;
        for (const FreeSpan &range : mFreeRanges)
            freeBytes += range.mSize;
        return mCapacityBytes - freeBytes;
    }
}
