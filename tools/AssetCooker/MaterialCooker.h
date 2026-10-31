/**
 * @file MaterialCooker.h
 * @author Jackey Cen
 * @brief .material.json parsing and the packed surface texture cook.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once
#include "GltfImporter.h"
#include <assets/ModelAsset.h>
#include <filesystem>
#include <string>
#include <vector>

namespace mir::cook
{
    struct MaterialTextureSize
    {
        std::string role;
        uint32_t width = 0, height = 0;
    };
    struct MaterialImport
    {
        AssetId materialId{};
        std::vector<ImportedBlob> blobs;
        std::vector<ImportedAsset> contents;
        std::vector<std::filesystem::path> dependencies;
        // The six source images, which the full cook must not also cook standalone.
        std::vector<std::filesystem::path> sourceImages;
        std::vector<MaterialTextureSize> textureSizes;
    };
    struct MaterialDependencies
    {
        std::vector<std::filesystem::path> files;
        std::vector<std::filesystem::path> images;
    };
    // Parses and resolves the v1 schema without decoding any pixel.
    MaterialDependencies InspectMaterialDependencies(const std::filesystem::path &path, const std::string &assetPath,
                                                     const std::filesystem::path &ownerRoot,
                                                     const std::vector<std::filesystem::path> &approvedRoots);
    // No output is published until parsing, resolution, decoding and packing all succeed.
    MaterialImport ImportMaterial(const std::filesystem::path &path, const std::string &assetPath,
                                  const std::filesystem::path &ownerRoot,
                                  const std::vector<std::filesystem::path> &approvedRoots);
} // namespace mir::cook
