/**
 * @file TerrainBaseCache.cpp
 * @author Jackey Cen
 * @brief Writes the generator's source heights and rule weights as local caches beside an asset's releases.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "TerrainBaseCache.h"

#include "TerrainReport.h"

#include <core/fs/FileIO.h>
#include <terrain/TerrainBaseHeights.h>
#include <terrain/TerrainBaseWeights.h>
#include <terrain/TerrainRelease.h>

#include <algorithm>
#include <format>
#include <memory>
#include <system_error>
#include <utility>
#include <vector>

namespace mir::terrainbake
{
    namespace
    {
        namespace fs = std::filesystem;

        bool EndsWith(std::string_view text, std::string_view tail)
        {
            return text.size() >= tail.size() && text.substr(text.size() - tail.size()) == tail;
        }

        bool PrepareStaging(const fs::path &root, std::string_view hex, fs::path &staging, std::string &error)
        {
            std::error_code ec;
            fs::create_directories(root, ec);
            if (ec)
            {
                error = std::format("cannot create {} ({})", PathUtf8(root), ec.message());
                return false;
            }
            // Under the publish lock every .partial and pointer temp is left from a dead writer.
            for (const fs::directory_entry &entry : fs::directory_iterator(root, ec))
            {
                const std::string name = PathUtf8(entry.path().filename());
                std::error_code ignored;
                if ((entry.is_directory(ignored) && EndsWith(name, ".partial")) || name == "current.tmp")
                    fs::remove_all(entry.path(), ignored);
            }

            staging = root / (std::string(hex) + ".partial");
            fs::remove_all(staging, ec);
            if (!fs::create_directory(staging, ec))
            {
                error = std::format("cannot create {} ({})", PathUtf8(staging), ec ? ec.message() : "it exists");
                return false;
            }
            return true;
        }

        bool InstallStaging(const fs::path &root, const fs::path &staging, const fs::path &finalDir,
                            const fs::path &pointer, std::string_view hex, std::string &error)
        {
            std::error_code ec;
            if (fs::exists(finalDir, ec))
            {
                fs::remove_all(finalDir, ec);
                if (ec)
                {
                    error = std::format("cannot remove the old cache {} ({})", PathUtf8(finalDir), ec.message());
                    return false;
                }
            }
            fs::rename(staging, finalDir, ec);
            if (ec)
            {
                error = std::format("cannot rename {} to {} ({})", PathUtf8(staging), PathUtf8(finalDir),
                                    ec.message());
                return false;
            }

            if (!WriteTextAtomic(pointer, hex))
            {
                error = std::format("cannot replace {}", PathUtf8(pointer));
                return false;
            }

            for (const fs::directory_entry &entry : fs::directory_iterator(root, ec))
            {
                const std::string name = PathUtf8(entry.path().filename());
                const std::optional<uint64_t> id = ParseTerrainIdHex(name);
                std::error_code isDir;
                if (!id || name == hex || !entry.is_directory(isDir))
                    continue;
                std::error_code ignored;
                fs::remove_all(entry.path(), ignored);
            }
            return true;
        }

        std::optional<std::string> PointerName(const fs::path &pointer)
        {
            std::optional<std::string> text = ReadText(pointer);
            if (!text)
                return std::nullopt;
            std::string_view name = *text;
            if (name.ends_with("\r\n"))
                name.remove_suffix(2);
            else if (name.ends_with('\n'))
                name.remove_suffix(1);
            return std::string(name);
        }
    }

    TerrainSourceFingerprint BaseHeightsFingerprint(const TerrainRecipe &recipe)
    {
        TerrainRecipe unedited = recipe;
        unedited.editStack.reset();
        unedited.editStackHash = 0;
        return FingerprintRecipe(unedited);
    }

    std::optional<TerrainEncodedRelease> EncodeBaseHeights(const TerrainRecipe &recipe, AssetId asset,
                                                          TerrainSourceFingerprint key,
                                                          std::span<const TileHeights> tiles, std::string &error)
    {
        if (key.IsNull())
        {
            error = "no base key";
            return std::nullopt;
        }
        if (tiles.empty())
        {
            error = "no source tiles";
            return std::nullopt;
        }
        const std::vector<TerrainLayout> layers = recipe.SourceLayers();
        std::vector<TerrainTileWrite> writes;
        writes.reserve(tiles.size());
        for (const TileHeights &tile : tiles)
        {
            const std::optional<double> spacing = recipe.SourceSpacing(tile.Tile());
            const auto layer = spacing ? std::find_if(layers.begin(), layers.end(),
                                                      [&](const TerrainLayout &candidate)
                                                      {
                                                          return candidate.SampleSpacing() == *spacing;
                                                      })
                                       : layers.end();
            if (!spacing || layer == layers.end())
            {
                error = std::format("tile ({}, {}) has no source layer", tile.Tile().x, tile.Tile().z);
                return std::nullopt;
            }
            writes.push_back({&tile, static_cast<uint32_t>(layer - layers.begin()), true, std::nullopt});
        }

        const TerrainReleaseInfo info{asset, key, kTerrainFormatVersion, TerrainReleaseId(key.value)};
        TerrainResult<TerrainEncodedRelease> encoded = EncodeTerrainRelease(info, layers, writes);
        if (!encoded)
        {
            error = encoded.error.message;
            return std::nullopt;
        }
        return std::move(*encoded.value);
    }

    bool BaseHeightsCurrent(const std::filesystem::path &releasesRoot, AssetId asset, TerrainSourceFingerprint key)
    {
        const std::optional<std::string> name = PointerName(TerrainBaseHeightsPointerPath(releasesRoot, asset));
        if (!name)
            return false;
        const std::optional<uint64_t> named = ParseTerrainIdHex(*name);
        std::error_code ec;
        return named && *named == key.value &&
               fs::is_regular_file(TerrainBaseHeightsDir(releasesRoot, asset, key) / kTerrainIndexFileName, ec);
    }

    bool WriteBaseHeights(const std::filesystem::path &releasesRoot, AssetId asset,
                          const TerrainBaseHeightsEncoding &base, std::string &error)
    {
        if (base.key.IsNull() || !base.encoded)
        {
            error = base.error.empty() ? "no base encoding" : base.error;
            return false;
        }

        const fs::path root = TerrainBaseHeightsRoot(releasesRoot, asset);
        const std::string hex = TerrainIdHex(base.key.value);
        fs::path staging;
        if (!PrepareStaging(root, hex, staging, error))
            return false;
        auto abandon = [&](std::string why)
        {
            std::error_code ignored;
            fs::remove_all(staging, ignored);
            error = std::move(why);
            return false;
        };
        if (!WriteBytes(staging / kTerrainIndexFileName, base.encoded->index))
            return abandon(std::format("cannot write {}", PathUtf8(staging / kTerrainIndexFileName)));
        for (const TerrainEncodedPack &pack : base.encoded->packs)
            if (!WriteBytes(staging / pack.fileName, pack.bytes))
                return abandon(std::format("cannot write {}", PathUtf8(staging / pack.fileName)));

        const std::shared_ptr<TerrainFileSource> source = std::make_shared<TerrainFileSource>(staging);
        TerrainResult<TerrainReleaseHandle> opened = TerrainReleaseHandle::Open(source);
        if (!opened)
            return abandon(std::format("cannot read back {}: {}", PathUtf8(staging), opened.error.message));
        const TerrainPackError verified = opened.value->VerifyAllPacks();
        if (verified.code != TerrainPackErrorCode::None)
            return abandon(std::format("cannot read back {}: {}", PathUtf8(staging), verified.message));
        const TerrainReleaseInfo &info = opened.value->Info();
        if (info.asset != asset || info.source != base.key || info.release.value != base.key.value)
            return abandon(std::format("{} was written with another asset, key or release", PathUtf8(staging)));

        if (!InstallStaging(root, staging, TerrainBaseHeightsDir(releasesRoot, asset, base.key),
                            TerrainBaseHeightsPointerPath(releasesRoot, asset), hex, error))
            return abandon(std::move(error));
        return true;
    }

    TerrainSourceFingerprint BaseWeightsFingerprint(const TerrainRecipe &recipe)
    {
        TerrainRecipe painted = recipe;
        painted.editWeightStackHash = 0;
        return FingerprintWeights(painted, FingerprintRecipe(recipe));
    }

    std::optional<TerrainEncodedBaseWeights> EncodeBaseWeights(const TerrainRecipe &recipe, AssetId asset,
                                                              TerrainSourceFingerprint key,
                                                              std::span<const TerrainWeightTile> tiles,
                                                              std::string &error)
    {
        if (key.IsNull())
        {
            error = "no base weights key";
            return std::nullopt;
        }
        if (tiles.empty())
        {
            error = "no rule weights";
            return std::nullopt;
        }
        std::vector<std::string> layers;
        layers.reserve(recipe.materials.size());
        for (const TerrainMaterialLayer &layer : recipe.materials)
            layers.push_back(layer.name);
        TerrainResult<TerrainEncodedBaseWeights> encoded =
            EncodeTerrainBaseWeights(asset, key, recipe.layout, layers, tiles);
        if (!encoded)
        {
            error = encoded.error.message;
            return std::nullopt;
        }
        return std::move(*encoded.value);
    }

    bool BaseWeightsCurrent(const std::filesystem::path &releasesRoot, AssetId asset, TerrainSourceFingerprint key)
    {
        const std::optional<std::string> name = PointerName(TerrainBaseWeightsPointerPath(releasesRoot, asset));
        if (!name)
            return false;
        const std::optional<uint64_t> named = ParseTerrainIdHex(*name);
        std::error_code ec;
        return named && *named == key.value &&
               fs::is_regular_file(TerrainBaseWeightsDir(releasesRoot, asset, key) / kTerrainBaseWeightsIndexFileName,
                                   ec);
    }

    bool WriteBaseWeights(const std::filesystem::path &releasesRoot, AssetId asset,
                          const TerrainBaseWeightsEncoding &base, std::string &error)
    {
        if (base.key.IsNull() || !base.encoded)
        {
            error = base.error.empty() ? "no base weights encoding" : base.error;
            return false;
        }

        const fs::path root = TerrainBaseWeightsRoot(releasesRoot, asset);
        const std::string hex = TerrainIdHex(base.key.value);
        fs::path staging;
        if (!PrepareStaging(root, hex, staging, error))
            return false;
        auto abandon = [&](std::string why)
        {
            std::error_code ignored;
            fs::remove_all(staging, ignored);
            error = std::move(why);
            return false;
        };
        if (!WriteBytes(staging / kTerrainBaseWeightsIndexFileName, base.encoded->index))
            return abandon(std::format("cannot write {}", PathUtf8(staging / kTerrainBaseWeightsIndexFileName)));
        if (!WriteBytes(staging / kTerrainBaseWeightsPackFileName, base.encoded->pack))
            return abandon(std::format("cannot write {}", PathUtf8(staging / kTerrainBaseWeightsPackFileName)));

        const TerrainBaseWeightsResult<TerrainBaseWeights> opened = TerrainBaseWeights::OpenDir(staging, asset);
        if (!opened)
            return abandon(std::format("cannot read back {}: {}", PathUtf8(staging), opened.error.message));
        if (opened.value->Key() != base.key)
            return abandon(std::format("{} was written with another key", PathUtf8(staging)));

        if (!InstallStaging(root, staging, TerrainBaseWeightsDir(releasesRoot, asset, base.key),
                            TerrainBaseWeightsPointerPath(releasesRoot, asset), hex, error))
            return abandon(std::move(error));
        return true;
    }
}
