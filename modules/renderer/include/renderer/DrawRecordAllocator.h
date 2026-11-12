/**
 * @file DrawRecordAllocator.h
 * @author Jackey Cen
 * @brief Sequential draw record indices for one frame.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <renderer/DrawRecord.h>

#include <cstdint>
#include <optional>

namespace mir
{
    // A surface material's pipeline reads gDrawRecords; a legacy one does not.
    constexpr bool NeedsDrawRecord(bool surfaceMaterial, bool hasMaterialRecord)
    {
        return surfaceMaterial && hasMaterialRecord;
    }

    // Reset at the start of a frame, then one index per draw that reads the records.
    class DrawRecordAllocator
    {
    public:
        void Reset() { mNext = 0; }

        std::optional<uint32_t> Acquire()
        {
            if (mNext >= kMaxDrawRecords)
                return std::nullopt;
            return mNext++;
        }

        uint32_t Used() const { return mNext; }

    private:
        uint32_t mNext = 0;
    };
}
