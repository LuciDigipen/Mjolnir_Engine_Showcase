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
#include <string>
#include <string_view>
#include <vector>

namespace mir
{
    enum class SurfaceTextureRole : uint32_t
    {
        Albedo,
        Normal,
        Surface,
        // The renderer's deterministic tileable low-frequency variation texture.
        Macro,
        Count
    };

    // Exact standard field names and their documented aliases.
    std::optional<SurfaceTextureRole> SurfaceTextureRoleForField(std::string_view field);

    // The packed float32 tileSizeMeters value from a material record. Nullopt
    // means the reflected type has no such field.
    std::optional<double> SurfaceMaterialTilePeriodFromRecord(const MaterialLayout &layout,
        std::span<const std::byte> record);

    struct SurfaceMaterialMacroParameters
    {
        // The reflected macro period is optional only so old materials report
        // "no macro" instead of a fabricated default.
        std::optional<double> periodMeters;
        double strength = 0.0;
        double hueShiftDegrees = 0.0;
        double roughnessContrast = 0.0;
    };

    struct SurfaceMaterialParallaxParameters
    {
        bool enabled = false;
        bool rayTracedEnabled = false;
        double heightMeters = 0.0;
        double reference = 0.0;
        double maxDistanceMeters = 0.0;
        uint32_t steps = 0;
        double fadeStartMeters = 0.0;
        double fadeEndMeters = 0.0;
    };

    // Nullopt when the reflected type declares no macro fields.
    std::optional<SurfaceMaterialMacroParameters> SurfaceMaterialMacroParametersFromRecord(
        const MaterialLayout &layout, std::span<const std::byte> record);
    // Nullopt when the reflected type declares no parallax fields.
    std::optional<SurfaceMaterialParallaxParameters> SurfaceMaterialParallaxParametersFromRecord(
        const MaterialLayout &layout, std::span<const std::byte> record);

    // Consumer-independent numeric bounds. Period divisibility stays with the
    // terrain consumer because it owns the validation period.
    bool ValidateSurfaceMaterialMacroParameters(const SurfaceMaterialMacroParameters &parameters,
        std::string *error);
    bool ValidateSurfaceMaterialParallaxParameters(const SurfaceMaterialParallaxParameters &parameters,
        std::string *error);

    // Deterministic, tileable, low-frequency RGBA8 macro data. Both lattice
    // periods divide the size, so the edges meet without a seam.
    void BuildSurfaceMacroTexture(uint32_t size, std::vector<std::byte> &rgba);
}
