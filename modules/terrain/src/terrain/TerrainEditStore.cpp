/**
 * @file TerrainEditStore.cpp
 * @author Jackey Cen
 * @brief Names, loads and plans the on-disk Edit Layer files of a terrain recipe.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "terrain/TerrainEditStore.h"

#include "PackInternal.h"

#include <core/fs/FileIO.h>

#include <algorithm>
#include <charconv>
#include <format>
#include <set>
#include <system_error>
#include <utility>

namespace mir
{
    namespace
    {
        using Code = TerrainPackErrorCode;
        using detail::Fail;
        using detail::Failure;
        using detail::LessZX;

        std::optional<int32_t> ParseCoordinate(std::string_view text)
        {
            int32_t value = 0;
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
            if (error != std::errc{} || end != text.data() + text.size())
                return std::nullopt;
            return value;
        }

        TerrainResult<std::vector<std::filesystem::path>> ListDirectory(const std::filesystem::path &dir)
        {
            using Result = std::vector<std::filesystem::path>;

            Result entries;
            std::error_code ec;
            for (const std::filesystem::directory_entry &entry : std::filesystem::directory_iterator(dir, ec))
                entries.push_back(entry.path());
            if (ec)
                return Fail<Result>(Code::Io, "{}: cannot list the edit layer directory", dir.generic_string());
            std::sort(entries.begin(), entries.end());
            return {std::move(entries), {}};
        }

        bool IsRegularFile(const std::filesystem::path &path)
        {
            std::error_code ec;
            return std::filesystem::is_regular_file(path, ec);
        }
    } // namespace

    std::filesystem::path TerrainEditLayersDir(const std::filesystem::path &recipePath)
    {
        std::string name = recipePath.filename().string();
        if (name.size() <= kTerrainRecipeSuffix.size() || !name.ends_with(kTerrainRecipeSuffix))
            return {};
        name.resize(name.size() - kTerrainRecipeSuffix.size());
        name.append(kTerrainEditLayersSuffix);
        return recipePath.parent_path() / name;
    }

    std::string TerrainEditLayerDirName(uint32_t sequence)
    {
        return std::format("L{}", sequence);
    }

    std::string TerrainEditTileFileName(TileCoord tile)
    {
        return std::format("tile_{}_{}{}", tile.x, tile.z, kTerrainEditTileSuffix);
    }

    std::optional<TileCoord> ParseTerrainEditTileFileName(std::string_view name)
    {
        constexpr std::string_view kPrefix = "tile_";
        if (!name.starts_with(kPrefix) || !name.ends_with(kTerrainEditTileSuffix))
            return std::nullopt;
        const std::string_view body = name.substr(kPrefix.size(), name.size() - kPrefix.size() -
                                                                   kTerrainEditTileSuffix.size());
        const size_t separator = body.find('_');
        if (separator == std::string_view::npos)
            return std::nullopt;
        const std::optional<int32_t> x = ParseCoordinate(body.substr(0, separator));
        const std::optional<int32_t> z = ParseCoordinate(body.substr(separator + 1));
        if (!x || !z)
            return std::nullopt;

        const TileCoord tile{*x, *z};
        if (TerrainEditTileFileName(tile) != name)
            return std::nullopt;
        return tile;
    }

    TerrainResult<std::vector<TerrainEditLayer>> LoadTerrainEditStack(const std::filesystem::path &layersDir)
    {
        using Result = std::vector<TerrainEditLayer>;

        std::error_code ec;
        if (!std::filesystem::exists(layersDir, ec))
            return {Result{}, {}};
        if (!std::filesystem::is_directory(layersDir, ec))
            return Fail<Result>(Code::Io, "{}: the edit layer path is not a directory", layersDir.generic_string());

        const std::filesystem::path manifestPath = layersDir / kTerrainEditManifestFileName;
        const TerrainResult<std::vector<std::filesystem::path>> entries = ListDirectory(layersDir);
        if (!entries)
            return Failure<Result>(entries.error, "");

        const std::optional<std::string> text = ReadText(manifestPath);
        if (!text)
            return Fail<Result>(Code::Io, "{}: the edit layer manifest is missing or unreadable",
                                manifestPath.generic_string());

        TerrainResult<std::vector<TerrainEditLayerInfo>> infos = DecodeTerrainEditManifest(*text);
        if (!infos)
            return Failure<Result>(infos.error, manifestPath.generic_string() + ": ");

        std::set<uint32_t> sequences;
        std::set<std::string> layerDirs;
        for (const TerrainEditLayerInfo &info : *infos.value)
        {
            if (!sequences.insert(info.sequence).second)
                return Fail<Result>(Code::Malformed, "{}: sequence {} is used by more than one layer",
                                    manifestPath.generic_string(), info.sequence);
            layerDirs.insert(TerrainEditLayerDirName(info.sequence));
        }

        for (const std::filesystem::path &entry : *entries.value)
        {
            const std::string name = entry.filename().string();
            if (name == kTerrainEditManifestFileName)
            {
                if (!IsRegularFile(entry))
                    return Fail<Result>(Code::Malformed, "{}: the edit layer manifest is not a file",
                                        entry.generic_string());
                continue;
            }
            std::error_code kind;
            if (!std::filesystem::is_directory(entry, kind) || !layerDirs.contains(name))
                return Fail<Result>(Code::Malformed, "{}: is not an edit layer file", entry.generic_string());
        }

        Result layers;
        layers.reserve(infos.value->size());
        for (const TerrainEditLayerInfo &info : *infos.value)
        {
            TerrainEditLayer layer;
            layer.info = info;

            const std::filesystem::path layerDir = layersDir / TerrainEditLayerDirName(info.sequence);
            std::error_code stat;
            const bool present = std::filesystem::exists(layerDir, stat);
            if (stat)
                return Fail<Result>(Code::Io, "{}: cannot inspect the layer directory", layerDir.generic_string());
            if (present && !std::filesystem::is_directory(layerDir, stat))
                return Fail<Result>(Code::Malformed, "{}: is not a layer directory", layerDir.generic_string());

            if (present)
            {
                const TerrainResult<std::vector<std::filesystem::path>> files = ListDirectory(layerDir);
                if (!files)
                    return Failure<Result>(files.error, "");

                for (const std::filesystem::path &file : *files.value)
                {
                    const std::optional<TileCoord> coord = ParseTerrainEditTileFileName(file.filename().string());
                    if (!coord || !IsRegularFile(file))
                        return Fail<Result>(Code::Malformed, "{}: is not an edit tile file", file.generic_string());

                    const std::optional<std::vector<std::byte>> bytes = ReadBytes(file);
                    if (!bytes)
                        return Fail<Result>(Code::Io, "{}: the edit tile cannot be read", file.generic_string());

                    TerrainResult<TerrainEditDecodedTile> decoded = DecodeTerrainEditTile(*bytes);
                    if (!decoded)
                        return Failure<Result>(decoded.error, file.generic_string() + ": ");

                    TerrainEditDecodedTile &tile = *decoded.value;
                    if (tile.tile.tile != *coord)
                        return Fail<Result>(Code::Corrupt, "{}: the tile blob holds tile ({}, {}), not ({}, {})",
                                            file.generic_string(), tile.tile.tile.x, tile.tile.tile.z, coord->x,
                                            coord->z);
                    if (tile.blend != info.heightBlend)
                        return Fail<Result>(
                            Code::Corrupt, "{}: the tile blend {} differs from the manifest layer's {}",
                            file.generic_string(), static_cast<uint32_t>(tile.blend),
                            static_cast<uint32_t>(info.heightBlend));
                    layer.tiles.emplace(*coord, std::move(tile.tile));
                }
            }
            layers.push_back(std::move(layer));
        }
        return {std::move(layers), {}};
    }

    TerrainResult<std::vector<TerrainEditStackFile>> PlanTerrainEditStackWrite(
        std::span<const TerrainEditLayer> layers)
    {
        using Result = std::vector<TerrainEditStackFile>;

        std::set<uint32_t> sequences;
        std::vector<TerrainEditLayerInfo> infos;
        infos.reserve(layers.size());
        for (const TerrainEditLayer &layer : layers)
        {
            if (!sequences.insert(layer.info.sequence).second)
                return Fail<Result>(Code::InvalidInput, "sequence {} is used by more than one layer",
                                    layer.info.sequence);
            infos.push_back(layer.info);
        }

        const std::string manifest = EncodeTerrainEditManifest(infos);
        const std::span<const std::byte> manifestBytes = std::as_bytes(std::span(manifest));
        Result files;
        files.push_back({std::filesystem::path(kTerrainEditManifestFileName),
                         std::vector<std::byte>(manifestBytes.begin(), manifestBytes.end())});

        for (const TerrainEditLayer &layer : layers)
        {
            std::vector<const TerrainEditTile *> tiles;
            tiles.reserve(layer.tiles.size());
            for (const auto &[coord, tile] : layer.tiles)
                tiles.push_back(&tile);
            std::sort(tiles.begin(), tiles.end(), [](const TerrainEditTile *left, const TerrainEditTile *right) {
                return LessZX(left->tile.x, left->tile.z, right->tile.x, right->tile.z);
            });

            const std::filesystem::path layerDir = TerrainEditLayerDirName(layer.info.sequence);
            for (const TerrainEditTile *tile : tiles)
            {
                if (tile->values.empty() && tile->weights.empty())
                    continue;
                const std::filesystem::path path = layerDir / TerrainEditTileFileName(tile->tile);
                TerrainResult<std::vector<std::byte>> encoded =
                    EncodeTerrainEditTile(*tile, layer.info.heightBlend);
                if (!encoded)
                    return Failure<Result>(encoded.error, path.generic_string() + ": ");
                files.push_back({path, std::move(*encoded.value)});
            }
        }
        return {std::move(files), {}};
    }
} // namespace mir
