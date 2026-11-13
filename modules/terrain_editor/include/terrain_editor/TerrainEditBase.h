/**
 * @file TerrainEditBase.h
 * @author Jackey Cen
 * @brief The base heights cache the brush edits against, and whether brushing is allowed.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <terrain_editor/TerrainEditTiles.h>

#include <assets/AssetId.h>
#include <terrain/TerrainBaseHeights.h>
#include <terrain/TerrainLayout.h>
#include <terrain/TileHeights.h>
#include <terrain/TileKey.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace mir
{
    using TerrainBaseTile = std::shared_ptr<const TileHeights>;
    using TerrainBaseTileProvider = std::function<TerrainBaseHeightsResult<TerrainBaseTile>(TileCoord)>;

    // One asset's cache, reopened after TerrainTool sync points it at another key.
    class TerrainBaseCache
    {
    public:
        // False with Error() set when the cache or a tile is unavailable.
        bool Ensure(const std::filesystem::path &releasesRoot, AssetId asset);
        void Clear();

        bool Ready() const { return mBase.has_value(); }
        AssetId Asset() const { return mAsset; }
        const std::string &Error() const { return mError; }
        // 0 while no cache is open.
        uint64_t KeyValue() const;

        std::optional<TerrainLayout> ReferenceLayout() const;
        // The tile's source grid in the release index.
        std::optional<TerrainLayout> Layout(TileCoord tile) const;
        TerrainBaseHeightsResult<TerrainBaseTile> ReadTile(TileCoord tile);
        TerrainBaseTileProvider Provider();

    private:
        bool Open(const std::filesystem::path &releasesRoot, AssetId asset);

        std::optional<TerrainBaseHeights> mBase;
        AssetId mAsset;
        std::string mError;
        std::optional<std::string> mPointerText;
        bool mTried = false;
        std::map<TileCoord, TerrainBaseTile, TerrainTileOrder> mTiles;
    };

    struct TerrainBrushAvailability
    {
        bool enabled = false;
        std::string message;
    };

    TerrainBrushAvailability EvaluateTerrainBrushAvailability(bool active, bool editing, bool sceneDocument,
                                                              bool documentEditable, bool hasLayer, bool baseReady,
                                                              std::string_view baseError);
} // namespace mir
