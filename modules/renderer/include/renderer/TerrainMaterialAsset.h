/**
 * @file TerrainMaterialAsset.h
 * @author Jackey Cen
 * @brief A layered terrain material's source JSON and its resolved layer materials.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <assets/AssetDecoder.h>
#include <assets/AssetId.h>
#include <renderer/SurfaceMaterialCatalog.h>
#include <renderer/TerrainLayerBlend.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mir
{
    struct TerrainMaterialLayerAsset
    {
        std::string name;
        // Repo-relative .material.json path once resolved against the owner's assets root.
        std::string material;
        AssetId asset;
    };

    struct TerrainMaterialAsset
    {
        TerrainLayerBlendMode blend = TerrainLayerBlendMode::Weight;
        float heightContrast = 0.1f;
        std::vector<TerrainMaterialLayerAsset> layers;
    };

    struct TerrainMaterialSource
    {
        std::string json;
    };

    const AssetDecoder<TerrainMaterialSource> &TerrainMaterialDecoder();

    bool IsTerrainMaterialPath(std::string_view path);

    // Validates the documented layered material shape. With a catalog, every layer must resolve to a
    // cooked single surface material that declares the terrain frontend.
    std::optional<TerrainMaterialAsset> ReadTerrainMaterial(std::string_view json, std::string_view ownerAsset,
        const SurfaceMaterialCatalog *catalog, std::string *error = nullptr);

    uint32_t TerrainLayerBlendModeValue(TerrainLayerBlendMode mode);
}
