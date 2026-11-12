/**
 * @file TerrainLayerWorkspace.h
 * @author Jackey Cen
 * @brief The scene's terrain assets and one Edit Layer document for each asset being edited.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace mir
{
    class TerrainLayerDocument;
    class TerrainWorld;

    struct TerrainEditAsset
    {
        std::string recipePath;
        std::string logicalPath;
    };

    class TerrainLayerWorkspace
    {
    public:
        explicit TerrainLayerWorkspace(std::filesystem::path assetsDir);
        ~TerrainLayerWorkspace();

        // The assets the world's instances use, in recipe path order; keeps the selection when it survives.
        void Sync(const TerrainWorld &world);
        void Clear();

        std::span<const TerrainEditAsset> Assets() const { return mAssets; }
        std::size_t SelectedAsset() const { return mSelectedAsset; }
        void SelectAsset(std::size_t index);

        // Null while the scene has no terrain asset; loaded from disk the first time it is asked for.
        TerrainLayerDocument *SelectedDocument();

        std::optional<uint32_t> SelectedLayer() const { return mSelectedLayer; }
        void SelectLayer(std::optional<uint32_t> sequence) { mSelectedLayer = sequence; }

    private:
        std::filesystem::path mAssetsDir;
        std::vector<TerrainEditAsset> mAssets;
        std::map<std::string, std::unique_ptr<TerrainLayerDocument>> mDocuments;
        std::size_t mSelectedAsset = 0;
        std::optional<uint32_t> mSelectedLayer;
    };
} // namespace mir
