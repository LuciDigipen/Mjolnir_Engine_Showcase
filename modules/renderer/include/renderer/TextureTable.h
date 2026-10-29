/**
 * @file TextureTable.h
 * @author Jackey Cen
 * @brief Stable image-table slots behind TextureHandles.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <renderer/Texture.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mir
{
    // A TextureHandle's index is its row in the shared image table. Removing a
    // row advances its generation, so every old handle to it stays stale even
    // after the row is reused.
    class TextureTable
    {
    public:
        static constexpr std::size_t kDefaultCapacity = 1024;

        explicit TextureTable(std::size_t capacity = kDefaultCapacity);

        // The next free row, or a null handle when the table is full.
        TextureHandle Add();
        // False for a null, stale or unknown handle.
        bool Remove(TextureHandle handle);
        bool Contains(TextureHandle handle) const;
        uint32_t Generation(std::size_t index) const;

        std::size_t Size() const { return mGenerations.size(); }
        std::size_t Capacity() const { return mCapacity; }

    private:
        std::size_t mCapacity = kDefaultCapacity;
        std::vector<uint32_t> mGenerations;
        std::vector<std::size_t> mFreeIndices;
    };
}
