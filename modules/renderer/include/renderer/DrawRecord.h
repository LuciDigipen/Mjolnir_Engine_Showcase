/**
 * @file DrawRecord.h
 * @author Jackey Cen
 * @brief CPU mirror of the per-draw record a surface draw selects through its base instance.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace mir
{
    constexpr uint32_t kMaxDrawRecords = 65536;

    // Must match DrawRecord in assets/shaders/SurfaceCommon.slang, including
    // its StructuredBuffer ArrayStride.
    struct DrawRecord
    {
        uint32_t materialOffset;
        uint32_t materialSize;
        uint32_t flags;
        uint32_t reserved;
        uint32_t terrain[4];
    };

    static_assert(std::is_standard_layout_v<DrawRecord>);
    static_assert(sizeof(DrawRecord) == 32);
    static_assert(offsetof(DrawRecord, materialOffset) == 0);
    static_assert(offsetof(DrawRecord, materialSize) == 4);
    static_assert(offsetof(DrawRecord, flags) == 8);
    static_assert(offsetof(DrawRecord, reserved) == 12);
    static_assert(offsetof(DrawRecord, terrain) == 16);
    static_assert(offsetof(DrawRecord, terrain) + 4 * sizeof(uint32_t) == sizeof(DrawRecord));
}
