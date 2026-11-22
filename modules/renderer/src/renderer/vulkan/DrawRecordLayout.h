/**
 * @file DrawRecordLayout.h
 * @author Jackey Cen
 * @brief The set and binding that carry the per-frame draw records.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <cstdint>

namespace mir::vk
{
    // Set 5 rides beside the material table and binds once per pass.
    constexpr uint32_t kDrawRecordSetIndex = 5;
    constexpr uint32_t kDrawRecordBinding = 0;
}
