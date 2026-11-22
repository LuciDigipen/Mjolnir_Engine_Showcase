/**
 * @file MaterialRecordTable.h
 * @author Jackey Cen
 * @brief Aligned raw-byte material records with frame-safe snapshots.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace mir
{
    struct MaterialRecordId
    {
        static constexpr uint32_t kInvalidIndex = UINT32_MAX;

        uint32_t mIndex = kInvalidIndex;
        uint32_t mGeneration = 0;

        constexpr bool IsNull() const { return mIndex == kInvalidIndex; }
        constexpr bool operator==(const MaterialRecordId &) const = default;
    };

    inline constexpr MaterialRecordId kNullMaterialRecord{};
    inline constexpr uint32_t kInvalidMaterialRecordOffset = UINT32_MAX;

    // Where one material's bytes sit in the shared table.
    struct MaterialRecordRef
    {
        uint32_t mOffset = kInvalidMaterialRecordOffset;
        uint32_t mSize = 0;
        uint32_t mFlags = 0;
        uint32_t mReserved = 0;
    };
    static_assert(sizeof(MaterialRecordRef) == 16, "MaterialRecordRef must stay one aligned row");

    struct MaterialRecordTableDesc
    {
        uint32_t capacityBytes = 1u << 20;
        uint32_t alignment = 16;
        uint32_t framesInFlight = 2;
    };

    // Two copies of every record: draws read the published copy while an update
    // is staged in the other. A published copy is retired only after the frames
    // that may still read it complete.
    class MaterialRecordTable
    {
    public:
        static constexpr uint32_t kCopies = 3;

        explicit MaterialRecordTable(const MaterialRecordTableDesc &desc = {});

        MaterialRecordId Create(std::span<const std::byte> bytes, uint32_t alignment = 0);
        // Rejected for a stale id or a different byte count.
        bool Update(MaterialRecordId id, std::span<const std::byte> bytes);
        bool Release(MaterialRecordId id);

        // Publishes staged updates for the frame being recorded. completedFrame
        // is the device timeline value after BeginFrame's wait.
        bool Publish(uint64_t completedFrame, uint64_t frame);

        uint32_t Offset(MaterialRecordId id) const;
        uint32_t Size(MaterialRecordId id) const;
        bool Valid(MaterialRecordId id) const;
        // The published copy, or an empty span for a stale id.
        std::span<const std::byte> PublishedBytes(MaterialRecordId id) const;

        uint32_t CapacityBytes() const { return mCapacityBytes; }
        uint32_t UsedBytes() const;

        struct Write
        {
            uint32_t mOffset = 0;
            std::span<const std::byte> mBytes;
        };

        struct OffsetChange
        {
            MaterialRecordId mId;
            uint32_t mOffset = kInvalidMaterialRecordOffset;
        };

        // Bytes to upload since the last Publish (and any Create). ClearWrites
        // lets the GPU owner consume them exactly once.
        std::span<const Write> Writes() const { return mWrites; }
        void ClearWrites() { mWrites.clear(); }
        // Records whose published offset changed at the last Publish (or Create).
        std::span<const OffsetChange> OffsetChanges() const { return mOffsetChanges; }

    private:
        struct Block
        {
            uint32_t mOffset = 0;
            uint32_t mStride = 0;
            uint32_t mSize = 0;
            uint32_t mGeneration = 0;
            uint32_t mActiveCopy = 0;
            bool mPending = false;
            bool mAlive = false;
            bool mReclaimPending = false;
            uint64_t mReclaimFrame = 0;
            uint64_t mCopyFreeAfter[kCopies]{};
            std::vector<std::byte> mPendingBytes;
        };

        struct FreeSpan
        {
            uint32_t mOffset = 0;
            uint32_t mSize = 0;
        };

        Block *FindBlock(MaterialRecordId id);
        const Block *FindBlock(MaterialRecordId id) const;
        bool AllocateRange(uint32_t bytes, uint32_t alignment, uint32_t &outOffset);
        void FreeRange(uint32_t offset, uint32_t size);
        void Reclaim(uint64_t completedFrame);
        uint32_t CopyOffset(const Block &block, uint32_t copy) const;

        uint32_t mCapacityBytes = 0;
        uint32_t mAlignment = 16;
        uint32_t mFramesInFlight = 2;
        uint64_t mCompletedFrame = 0;
        uint64_t mFrame = 0;

        std::vector<std::byte> mStorage;
        std::vector<FreeSpan> mFreeRanges;
        std::vector<Block> mBlocks;
        std::vector<uint32_t> mFreeBlocks;
        std::vector<Write> mWrites;
        std::vector<OffsetChange> mOffsetChanges;
    };
}
