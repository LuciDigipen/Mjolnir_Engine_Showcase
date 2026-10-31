/**
 * @file GltfImporter.h
 * @author Jackey Cen
 * @brief glTF/GLB to typed model assets, with stable identity sidecars and package inventory.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once
#include <assets/AssetManifest.h>
#include <assets/AssetMeta.h>
#include <filesystem>
#include <string>
#include <vector>

namespace mir::cook
{
    struct ImportedBlob
    {
        AssetId id;
        std::vector<std::byte> bytes;
    };
    // One entry per addressable thing inside the package, for the editor's
    // import panel. A scene is not its own blob, so its id stays null.
    struct ImportedAsset
    {
        AssetId id;
        std::string kind, name;
        uint32_t index = 0;
    };
    struct GltfImport
    {
        AssetId modelId{};
        std::vector<std::filesystem::path> dependencies;
        std::vector<ImportedBlob> blobs;
        std::vector<ImportedAsset> contents;
        // the importer state for the .meta, the part keys handed out so far
        std::string importer;
    };
    // No output is published until all conversion/validation has succeeded.
    // meta is the package's .meta: its guid is the model's id and its importer state keeps the keys stable
    GltfImport ImportGltf(const std::filesystem::path &path, const std::string &assetPath,
                          const std::vector<std::filesystem::path> &cookedRoots, const AssetMeta &meta);

    // Resolves URI dependencies without loading them. Every returned path is
    // canonical and confined to one of allowedRoots.
    std::vector<std::filesystem::path> InspectGltfDependencies(const std::filesystem::path &path,
                                                               const std::vector<std::filesystem::path> &allowedRoots);
} // namespace mir::cook
