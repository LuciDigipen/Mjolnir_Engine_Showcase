/**
 * @file TextureTable.cpp
 * @author Jackey Cen
 * @brief Stable image-table slots behind TextureHandles.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer/TextureTable.h>

namespace mir
{
    TextureTable::TextureTable(std::size_t capacity)
        : mCapacity(capacity)
    {}

    TextureHandle TextureTable::Add()
    {
        if (!mFreeIndices.empty())
        {
            const std::size_t index = mFreeIndices.back();
            mFreeIndices.pop_back();
            return TextureHandle{ static_cast<uint32_t>(index), mGenerations[index] };
        }

        if (mGenerations.size() >= mCapacity)
            return kNullTexture;

        const uint32_t index = static_cast<uint32_t>(mGenerations.size());
        mGenerations.push_back(1);
        return TextureHandle{ index, 1 };
    }

    bool TextureTable::Remove(TextureHandle handle)
    {
        if (!Contains(handle))
            return false;

        ++mGenerations[handle.mIndex];
        mFreeIndices.push_back(handle.mIndex);
        return true;
    }

    bool TextureTable::Contains(TextureHandle handle) const
    {
        return !handle.IsNull() && handle.mIndex < mGenerations.size() &&
            mGenerations[handle.mIndex] == handle.mGeneration;
    }

    uint32_t TextureTable::Generation(std::size_t index) const
    {
        return index < mGenerations.size() ? mGenerations[index] : 0;
    }
}
