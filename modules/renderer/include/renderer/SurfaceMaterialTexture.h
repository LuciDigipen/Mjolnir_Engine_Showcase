/**
 * @file SurfaceMaterialTexture.h
 * @author Jackey Cen
 * @brief Packed surface texture roles and their reflected field names.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <renderer/MaterialReflection.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace mir
{
    enum class SurfaceTextureRole : uint32_t
    {
        Albedo,
        Normal,
        Surface,
        Count
    };

    // Exact standard field names and their documented aliases.
    std::optional<SurfaceTextureRole> SurfaceTextureRoleForField(std::string_view field);

    // The packed float32 tileSizeMeters value from a material record. Nullopt
    // means the reflected type has no such field.
    std::optional<double> SurfaceMaterialTilePeriodFromRecord(const MaterialLayout &layout,
        std::span<const std::byte> record);
}
