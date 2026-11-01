/**
 * @file SurfaceMaterialCatalog.h
 * @author Jackey Cen
 * @brief Build-generated surface material registrations and their records.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mir
{
    struct SurfaceMaterialRegistration
    {
        std::string stem;
        std::string type;
        std::string frontend;
        // Normalized repo-relative material and shader paths; the asset keys
        // separate two materials that share one compiled shader.
        std::string asset;
        std::string shader;
        std::string parametersJson = "{}";
        std::string reflectionFile;
    };

    struct SurfaceMaterialCatalog
    {
        std::vector<SurfaceMaterialRegistration> materials;

        const SurfaceMaterialRegistration *Find(std::string_view stem) const;
        const SurfaceMaterialRegistration *Find(std::string_view stem, std::string_view asset) const;
        const SurfaceMaterialRegistration *FindByAsset(std::string_view asset) const;
        // The same asset may declare one registration per frontend.
        const SurfaceMaterialRegistration *FindByAsset(std::string_view asset, std::string_view frontend) const;
        // Registration index for the primary/shadow SBT pair; UINT32_MAX when absent.
        uint32_t IndexOf(const SurfaceMaterialRegistration *registration) const;
        // Primary and shadow records are adjacent, so a surface instance's
        // base offset is even and TraceRay's 0/1 record offset lands inside.
        static constexpr uint32_t HitGroupRecordOffset(std::size_t index) { return 2 + static_cast<uint32_t>(index) * 2; }
    };

    // Reads shaders/surface_materials.json beside the executable. Missing or
    // malformed registrations are an explicit error, never a default fallback.
    std::optional<SurfaceMaterialCatalog> LoadSurfaceMaterialCatalog(std::string *error = nullptr);

    // Parses the registration's reflection witness and packs parameters from
    // the authored JSON. This is the only production record initializer.
    std::optional<std::vector<std::byte>> PackSurfaceMaterialRecord(
        const SurfaceMaterialRegistration &registration, std::string *error = nullptr);
}
