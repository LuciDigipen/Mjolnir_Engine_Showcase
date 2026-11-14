/**
 * @file TerrainBake.cpp
 * @author Jackey Cen
 * @brief Bakes a terrain recipe into final tile heights, an encoded release and a candidate directory.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "TerrainBake.h"
#include "TerrainDerivatives.h"
#include "TerrainIncremental.h"

#include "TerrainFinalSurface.h"
#include "TerrainGenerator.h"
#include "TerrainHeightSource.h"
#include "TerrainProgress.h"
#include "TerrainSeams.h"
#include "TerrainValidate.h"

#include <assets/AssetPath.h>
#include <jobs/JobExecutor.h>
#include <terrain/TerrainEditLayer.h>
#include <terrain/TerrainPackFormat.h>
#include <terrain/TerrainRegionWeights.h>
#include <terrain/TerrainRelease.h>
#include <terrain/TerrainSurface.h>

#include <algorithm>
#include <atomic>
#include <format>
#include <set>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace mir::terrainbake
{
    namespace
    {
        namespace fs = std::filesystem;

        std::string_view SourceStatusName(SourceFillStatus status)
        {
            switch (status)
            {
            case SourceFillStatus::Ok:
                return "ok";
            case SourceFillStatus::OutsideMap:
                return "outside the map";
            case SourceFillStatus::EmptyRange:
                return "an empty range";
            case SourceFillStatus::WrongSize:
                return "a wrong block size";
            case SourceFillStatus::NonFinite:
                return "a non-finite height";
            case SourceFillStatus::InvalidGrid:
                return "a grid that does not nest in the reference grid";
            }
            return "?";
        }

        void Fail(TerrainReport &report, std::string_view check, std::string field, std::string message)
        {
            report.Add({std::string(check), std::move(field), {}, std::nullopt, std::move(message)});
        }

        // Removes the staging directory on every return and exception unless cleared after the rename.
        struct StagingCleanup
        {
            fs::path dir;

            ~StagingCleanup()
            {
                std::error_code ignored;
                if (!dir.empty())
                    fs::remove_all(dir, ignored);
            }
        };

        char Lower(char c)
        {
            return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        }

        bool SameIgnoringCase(std::string_view a, std::string_view b)
        {
            return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(),
                                                      [](char x, char y)
                                                      {
                                                          return Lower(x) == Lower(y);
                                                      });
        }

        bool CheckArguments(const TerrainBakeRequest &request, const fs::path &out, std::string &assetPath,
                            TerrainReport &report)
        {
            using check::kArguments;
            if (const std::optional<TerrainAssetPath> asset =
                    CheckRecipeArguments(request.recipePath, request.idRoot, request.sourcePath, report))
                assetPath = asset->logicalPath;

            std::error_code ec;
            if (out.empty())
                Fail(report, kArguments, "--out", "no candidate directory");
            else if (fs::exists(out, ec))
            {
                if (!fs::is_directory(out, ec))
                    Fail(report, kArguments, "--out", std::format("{} exists and is not a directory", PathUtf8(out)));
                else if (!fs::is_empty(out, ec))
                    Fail(report, kArguments, "--out",
                         std::format("{} exists and is not empty; a bake never writes into or replaces an existing "
                                     "directory (remove the old candidate or choose another --out)",
                                     PathUtf8(out)));
            }
            if (report.Status(kArguments) == CheckStatus::Failed)
                return false;
            report.Ran(kArguments, std::format("asset {} ({})", assetPath, TerrainIdHex(MakeAssetId(assetPath).value)));
            return true;
        }
    }

    std::optional<std::string> RecipeSpellingMismatch(const fs::path &recipePath, std::string_view sourcePath)
    {
        const AssetPathResult normalized = NormalizeAssetPath(sourcePath);
        if (!normalized.Ok())
            return std::nullopt;
        const std::string_view wanted = std::string_view(normalized.path).substr(normalized.path.rfind('/') + 1);
        // The listing has the file's own spelling; the given path keeps the caller's.
        std::error_code ec;
        const fs::path parent = recipePath.has_parent_path() ? recipePath.parent_path() : fs::path(".");
        for (const fs::directory_entry &entry : fs::directory_iterator(parent, ec))
        {
            const std::string stored = PathUtf8(entry.path().filename());
            if (SameIgnoringCase(stored, PathUtf8(recipePath.filename())))
            {
                if (stored != wanted && SameIgnoringCase(stored, wanted))
                    return std::format("the recipe file is \"{}\" on disk but --source-path names \"{}\"; asset ids "
                                       "keep case, so this would publish an asset that a reference spelled like the "
                                       "file never finds (rename one to match)",
                                       stored, wanted);
                break;
            }
        }
        return std::nullopt;
    }

    std::optional<TerrainAssetPath> CheckRecipeArguments(const fs::path &recipePath, std::string_view idRoot,
                                                         std::string_view sourcePath, TerrainReport &report)
    {
        using check::kArguments;
        bool ok = true;
        if (recipePath.empty())
        {
            Fail(report, kArguments, "--recipe", "no recipe file");
            ok = false;
        }
        TerrainAssetPath asset = ResolveTerrainAssetPath(idRoot, sourcePath);
        if (!asset.Ok())
        {
            Fail(report, kArguments, asset.error == TerrainAssetPathError::IdRoot ? "--id-root" : "--source-path",
                 DescribeTerrainAssetPathError(asset, idRoot, sourcePath));
            return std::nullopt;
        }
        if (std::optional<std::string> mismatch = RecipeSpellingMismatch(recipePath, sourcePath))
        {
            Fail(report, kArguments, "--source-path", std::move(*mismatch));
            ok = false;
        }
        if (!ok)
            return std::nullopt;
        return asset;
    }

    std::optional<std::vector<TileHeights>> GenerateSourceHeights(const TerrainRecipe &recipe, TerrainReport &report)
    {
        if (recipe.IsHeightmap() && !recipe.heightSource)
        {
            report.Add({std::string(check::kGenerate), "parameters.source", {}, std::nullopt, "height source not loaded"});
            return std::nullopt;
        }
        const TerrainGenerator generator(recipe);
        const TerrainLayout &layout = generator.Layout();
        const TileRange range = layout.Tiles();
        std::vector<TileHeights> tiles;
        report.sourceEstimates = recipe.SourceEstimates();
        for (int64_t tz = range.min.z; tz <= range.max.z; ++tz)
            for (int64_t tx = range.min.x; tx <= range.max.x; ++tx)
            {
                const int32_t x = static_cast<int32_t>(tx), z = static_cast<int32_t>(tz);
                const auto sourceLayout = recipe.SourceLayout({x, z});
                if (!sourceLayout)
                {
                    report.Add({std::string(check::kGenerate),
                                "precision.tiles",
                                {{x, z}},
                                std::nullopt,
                                "tile has no source spacing"});
                    return std::nullopt;
                }
                std::optional<TileHeights> tile = TileHeights::Create({x, z}, sourceLayout->IntervalsPerTile());
                if (!tile)
                {
                    report.Add({std::string(check::kGenerate),
                                "layout.intervalsPerTile",
                                {{x, z}},
                                std::nullopt,
                                std::format("{} intervals per tile is beyond a tile block's limit {}",
                                            layout.IntervalsPerTile(), kMaxTerrainTileIntervals)});
                    return std::nullopt;
                }
                TerrainProgressStep("generate: tiles", tiles.size(),
                                    uint64_t(int64_t(range.max.x) - range.min.x + 1) *
                                        uint64_t(int64_t(range.max.z) - range.min.z + 1));
                const SourceFillResult filled = generator.FillTile(*sourceLayout, {x, z}, tile->Samples());
                report.referenceSamples += filled.referenceSamples;
                report.peakDoubleSamples = std::max(report.peakDoubleSamples, filled.peakDoubleSamples);
                if (!filled.Ok())
                {
                    const bool atSample = filled.status == SourceFillStatus::NonFinite;
                    report.Add({std::string(atSample ? check::kFinite : check::kGenerate),
                                "samples",
                                {{x, z}},
                                atSample ? std::optional(filled.sample) : std::nullopt,
                                std::format("the generator returned {} for tile ({}, {})",
                                            SourceStatusName(filled.status), x, z)});
                    return std::nullopt;
                }
                tiles.push_back(std::move(*tile));
            }
        report.Ran(check::kGenerate,
                   std::format("{} source tiles from \"{}\" v{}; fixed reference {} m, tile {} m; {}", tiles.size(),
                               recipe.generatorId, recipe.generatorVersion, layout.SampleSpacing(), layout.TileSize(),
                               recipe.recipeVersion == 1 ? "uniform P1 heights unchanged" : "filtered source grids"));
        return tiles;
    }

    namespace
    {
        bool ApplyEditsAndSeams(const TerrainRecipe &recipe, std::vector<TileHeights> &tiles, TerrainReport &report)
        {
            if (recipe.editStack && !recipe.editStack->empty())
                for (TileHeights &tile : tiles)
                {
                    const TileCoord at = tile.Tile();
                    TerrainResult<TileHeights> composed = CompositeTerrainEditLayers(tile, *recipe.editStack);
                    if (!composed)
                    {
                        report.Add({std::string(check::kGenerate),
                                    "edits",
                                    {at},
                                    std::nullopt,
                                    std::format("tile ({}, {}): {}", at.x, at.z, composed.error.message)});
                        return false;
                    }
                    tile = std::move(*composed.value);
                }
            TerrainProgressStage("seams");
            return AdjustTerrainSeams(recipe, tiles, report);
        }

        std::optional<std::vector<TileHeights>> BakeCandidateHeights(const TerrainRecipe &recipe, AssetId asset,
                                                                     TerrainBaseHeightsEncoding *base,
                                                                     TerrainReport &report)
        {
            std::optional<std::vector<TileHeights>> tiles = GenerateSourceHeights(recipe, report);
            if (!tiles)
                return std::nullopt;
            if (base)
            {
                base->key = BaseHeightsFingerprint(recipe);
                std::string error;
                base->encoded = EncodeBaseHeights(recipe, asset, base->key, *tiles, error);
                base->error = std::move(error);
            }
            if (!ApplyEditsAndSeams(recipe, *tiles, report))
                return std::nullopt;
            return tiles;
        }

        struct WeightSlot
        {
            std::optional<TerrainWeightTile> weights;
            std::vector<std::string> skipped;
            std::string error;
        };

        std::optional<std::vector<TerrainWeightTile>> BakeTerrainWeights(const TerrainRecipe &recipe,
                                                                         std::span<const TileHeights> heights,
                                                                         TerrainReport &report)
        {
            if (!recipe.hasMaterialsSection)
                return std::vector<TerrainWeightTile>{};

            TerrainFinalSurface surface{recipe, {}};
            for (const TileHeights &tile : heights)
                surface.tiles.emplace(std::pair(tile.Tile().z, tile.Tile().x), &tile);

            std::vector<WeightSlot> slots(heights.size());
            const bool hasStack = recipe.editStack && !recipe.editStack->empty();
            JobExecutor executor(JobExecutor::DefaultWorkerCount());
            std::atomic<std::size_t> completed{0};
            const auto caller = std::this_thread::get_id();
            TerrainProgressStage("weights");
            executor.ParallelFor(heights.size(), 1,
                                 [&](std::size_t begin, std::size_t end)
                                 {
                                     for (std::size_t index = begin; index < end; ++index)
                                     {
                                         WeightSlot &slot = slots[index];
                                         const TileCoord tile = heights[index].Tile();
                                         std::optional<TerrainWeightTile> weights =
                                             EvaluateTerrainWeights(recipe, surface, tile);
                                         if (!weights)
                                         {
                                             slot.error = std::format("tile ({}, {}) has no samples on the final "
                                                                      "surface",
                                                                      tile.x, tile.z);
                                             continue;
                                         }
                                         if (hasStack)
                                         {
                                             TerrainResult<TerrainEditWeightComposite> composite =
                                                 CompositeTerrainEditWeights(*weights, *recipe.editStack);
                                             if (!composite)
                                             {
                                                 slot.error = std::format("tile ({}, {}): {}", tile.x, tile.z,
                                                                          composite.error.message);
                                                 continue;
                                             }
                                             slot.skipped = std::move(composite.value->skippedLayers);
                                             weights = std::move(composite.value->tile);
                                         }
                                         slot.weights = std::move(weights);
                                         const std::size_t done =
                                             completed.fetch_add(1, std::memory_order_relaxed) + 1;
                                         // The progress sink is not thread safe, so only the caller reports.
                                         if (std::this_thread::get_id() == caller)
                                             TerrainProgressStep("weights: tiles", done, heights.size());
                                     }
                                 });
            TerrainProgressStep("weights: tiles", heights.size(), heights.size());

            std::set<std::string> skipped;
            for (const WeightSlot &slot : slots)
            {
                if (!slot.error.empty())
                {
                    report.Add({std::string(check::kEncode), "weights", {}, std::nullopt, slot.error});
                    return std::nullopt;
                }
                skipped.insert(slot.skipped.begin(), slot.skipped.end());
            }
            if (!skipped.empty())
            {
                std::string names;
                for (const std::string &name : skipped)
                    names += (names.empty() ? "" : ", ") + name;
                report.Warn({std::string(check::kEncode), "editWeights", {}, std::nullopt,
                             std::format("edit layer weight channels name material layers the recipe does not have; "
                                         "skipped: {}",
                                         names)});
            }

            std::vector<TerrainWeightTile> weights;
            weights.reserve(heights.size());
            for (WeightSlot &slot : slots)
                weights.push_back(std::move(*slot.weights));
            return weights;
        }

        // From the encoded tile payloads, so a validation that decodes the release derives the same images.
        std::optional<std::vector<TerrainRegionWeightImage>>
        BakeRegionWeightImages(const TerrainRegionGrid &grid, std::span<const std::string> table,
                               std::span<const TerrainWeightTile> tiles, TerrainReport &report)
        {
            std::vector<TerrainWeightTile> quantised;
            quantised.reserve(tiles.size());
            for (const TerrainWeightTile &tile : tiles)
            {
                TerrainResult<TerrainEncodedWeights> encoded = EncodeTerrainWeightPayload(tile, table);
                if (!encoded)
                {
                    report.Add({std::string(check::kEncode), "weights", {}, std::nullopt, encoded.error.message});
                    return std::nullopt;
                }
                TerrainDecodedWeights decoded;
                decoded.tile = tile.tile;
                decoded.intervals = tile.intervals;
                decoded.slots = encoded.value->slots;
                for (size_t s = 0; s < kTerrainWeightSlots && decoded.slots[s] != 0xFF; ++s)
                    decoded.layers[s] = table[decoded.slots[s]];
                decoded.bytes = std::move(encoded.value->bytes);
                quantised.push_back(ExpandTerrainWeights(decoded));
            }
            std::unordered_map<TileCoord, const TerrainWeightTile *> byTile;
            for (const TerrainWeightTile &tile : quantised)
                byTile.emplace(tile.tile, &tile);
            const TerrainWeightTileLookup lookup = [&byTile](TileCoord at) -> const TerrainWeightTile *
            {
                const auto it = byTile.find(at);
                return it == byTile.end() ? nullptr : it->second;
            };

            TerrainProgressStage("region weights");
            TerrainResult<std::vector<TerrainRegionWeightImage>> images =
                BuildTerrainRegionWeights(grid, table, lookup,
                                          {.side = kTerrainRegionWeightSide,
                                           .workers = std::max(1u, std::thread::hardware_concurrency())});
            if (!images)
            {
                report.Add({std::string(check::kEncode), "regionWeights", {}, std::nullopt, images.error.message});
                return std::nullopt;
            }
            TerrainProgressStep("region weights: regions", images.value->size(), images.value->size());
            return std::move(*images.value);
        }
    }

    std::optional<std::vector<TileHeights>> BakeFinalHeights(const TerrainRecipe &recipe, TerrainReport &report)
    {
        std::optional<std::vector<TileHeights>> tiles = GenerateSourceHeights(recipe, report);
        if (!tiles)
            return std::nullopt;
        if (!ApplyEditsAndSeams(recipe, *tiles, report))
            return std::nullopt;
        return tiles;
    }

    bool LoadRecipeHeightSource(TerrainRecipe &recipe, TerrainReport &report)
    {
        if (!recipe.IsHeightmap() || recipe.heightSource)
            return true;
        TerrainProgressStage("load heights");
        std::string error;
        const fs::path source = recipe.recipeDir /
            fs::path(std::u8string_view(reinterpret_cast<const char8_t *>(recipe.heightmapPath.data()),
                                        recipe.heightmapPath.size()));
        recipe.heightSource = TerrainHeightSource::Load(source, recipe.heightmapHash, recipe.layout, error);
        if (!recipe.heightSource)
        {
            report.Add({std::string(check::kGenerate), "parameters.source", {}, std::nullopt, std::move(error)});
            return false;
        }
        return true;
    }

    std::optional<TerrainBakedRelease> EncodeBakedRelease(AssetId asset, TerrainSourceFingerprint source,
                                                          const TerrainLayout &layout, std::vector<TileHeights> tiles,
                                                          const TerrainBakeOptions &options, TerrainReport &report)
    {
        const uint64_t heightsHash = TerrainHeightsHash(layout, tiles);
        const uint64_t nonce = options.nonce ? *options.nonce : MakeTerrainReleaseNonce();
        const TerrainReleaseInfo info{asset, source, kTerrainFormatVersion,
                                      MintTerrainReleaseId(asset, source, heightsHash, nonce)};
        TerrainResult<TerrainEncodedRelease> encoded = EncodeTerrainRelease(info, layout, tiles, options.pack);
        if (!encoded)
        {
            Fail(report, check::kEncode, "", encoded.error.message);
            return std::nullopt;
        }
        report.Ran(check::kEncode,
                   std::format("release {}: index and {} packs, heights hash {}", TerrainIdHex(info.release.value),
                               encoded.value->packs.size(), TerrainIdHex(encoded.value->heightsHash)));
        return TerrainBakedRelease{info, layout, nonce, std::move(tiles), std::move(*encoded.value), report.seams};
    }

    std::optional<TerrainBakedRelease> EncodeBakedRelease(AssetId asset, TerrainSourceFingerprint source,
                                                          const TerrainRecipe &recipe, std::vector<TileHeights> tiles,
                                                          const TerrainBakeOptions &options, TerrainReport &report,
                                                          const TerrainReleaseHandle *baseRelease,
                                                          std::string baseUnavailable)
    {
        if (recipe.recipeVersion != 1 && !report.seams)
        {
            Fail(report, check::kEncode, "seams", "v2 final heights require a seam summary");
            return std::nullopt;
        }
        if (report.seams &&
            (report.seams->algorithmVersion != kTerrainSeamVersion ||
             report.seams->bandMultiplier != kTerrainSeamBandMultiplier || report.seams->weight != "smoothstep" ||
             report.seams->warningThreshold != kTerrainSeamWarningThreshold))
        {
            Fail(report, check::kEncode, "seams", "prototype seam options cannot be encoded as a production candidate");
            return std::nullopt;
        }

        std::optional<std::vector<TileCoord>> affected;
        std::string mode = "full bake";
        if (baseRelease)
        {
            if (baseRelease->Info().asset != asset)
                mode = "full bake because the base release belongs to another asset";
            else if (const std::optional<std::string> mismatch =
                         TerrainBaseMismatch(*baseRelease, recipe, tiles, options.regions))
                mode = "full bake because " + *mismatch;
            else
            {
                std::size_t changed = 0;
                affected = TerrainAffectedTiles(*baseRelease, tiles, changed);
                mode = std::format("incremental: {} changed, {} tiles rederived, {} reused", changed,
                                   affected->size(), tiles.size() - affected->size());
            }
        }
        else if (!baseUnavailable.empty())
            mode = "full bake because " + baseUnavailable;
        std::unordered_set<TileCoord> onlySet;
        const std::unordered_set<TileCoord> *onlyTiles = nullptr;
        if (affected)
        {
            onlySet.insert(affected->begin(), affected->end());
            onlyTiles = &onlySet;
        }

        std::string derivativeError;
        auto derivatives =
            BakeTerrainDerivatives(recipe, tiles, derivativeError, &options.regions, std::nullopt, onlyTiles);
        if (!derivatives)
        {
            Fail(report, check::kEncode, "derivatives", derivativeError);
            return {};
        }
        std::vector<TerrainPayloadWrite> payloads = std::move(derivatives->payloads);
        std::vector<TileHeights> queryHeights = std::move(derivatives->queryHeights);
        std::vector<uint32_t> queryLayers = std::move(derivatives->queryLayers);
        std::vector<double> queryErrors = std::move(derivatives->queryErrors);
        if (baseRelease && affected)
        {
            for (const TerrainTileRecord &record : baseRelease->Index().tiles)
            {
                if (record.source || record.kind == TerrainPayloadKind::Region || onlySet.contains(record.tile))
                    continue;
                if (record.kind == TerrainPayloadKind::Height)
                {
                    TerrainResult<TileHeights> query = baseRelease->ReadTileRecord(record);
                    if (!query)
                    {
                        Fail(report, check::kEncode, "derivatives", query.error.message);
                        return std::nullopt;
                    }
                    queryHeights.push_back(std::move(*query.value));
                    queryLayers.push_back(record.layer);
                    queryErrors.push_back(record.maxDeviation.value_or(0.0));
                }
                else
                {
                    TerrainResult<std::vector<std::byte>> bytes = baseRelease->ReadPayload(record);
                    if (!bytes)
                    {
                        Fail(report, check::kEncode, "derivatives", bytes.error.message);
                        return std::nullopt;
                    }
                    payloads.push_back({record, std::move(*bytes.value)});
                }
            }
        }
        const auto &layers = derivatives->layers;
        std::vector<TerrainTileWrite> records;
        for (const auto &tile : tiles)
        {
            const auto spacing = recipe.SourceSpacing(tile.Tile());
            const auto layer = std::find_if(layers.begin(), layers.end(),
                                            [&](const auto &layout)
                                            {
                                                return spacing && layout.SampleSpacing() == *spacing;
                                            });
            if (layer == layers.end())
            {
                Fail(report, check::kEncode, "precision.tiles", "tile has no assigned source layer");
                return std::nullopt;
            }
            records.push_back({&tile, static_cast<uint32_t>(layer - layers.begin()), true, 0.0});
        }
        std::unordered_map<TileCoord, uint64_t> sourceChecksums;
        for (const auto &tile : tiles)
            sourceChecksums.emplace(tile.Tile(), TerrainFinalHeightChecksum(tile));
        for (size_t i = 0; i < queryHeights.size(); ++i)
            records.push_back({&queryHeights[i], queryLayers[i], false, queryErrors[i],
                               sourceChecksums.at(queryHeights[i].Tile())});
        const uint64_t heightsHash =
            TerrainPayloadsHash(layers, records, payloads, derivatives->regions->writes);
        const uint64_t nonce = options.nonce ? *options.nonce : MakeTerrainReleaseNonce();
        const TerrainReleaseInfo info{asset, source, kTerrainFormatVersion,
                                      MintTerrainReleaseId(asset, source, heightsHash, nonce)};

        std::vector<TerrainWeightTile> weightTiles;
        std::vector<TerrainRegionWeightImage> regionWeightImages;
        TerrainWeightsWrites weights;
        if (recipe.hasMaterialsSection)
        {
            std::optional<std::vector<TerrainWeightTile>> bakedWeights = BakeTerrainWeights(recipe, tiles, report);
            if (!bakedWeights)
                return std::nullopt;
            weightTiles = std::move(*bakedWeights);
            for (const TerrainMaterialLayer &layer : recipe.materials)
                weights.layers.push_back(layer.name);
            weights.fingerprint = FingerprintWeights(recipe, source).value;
            weights.tiles = std::span<const TerrainWeightTile>(weightTiles);
            if (derivatives->regions && derivatives->regions->writes.grid)
            {
                std::optional<std::vector<TerrainRegionWeightImage>> images = BakeRegionWeightImages(
                    *derivatives->regions->writes.grid, weights.layers, weightTiles, report);
                if (!images)
                    return std::nullopt;
                regionWeightImages = std::move(*images);
                weights.regionImages = regionWeightImages;
            }
        }

        TerrainProgressStage("encode");
        auto encoded = EncodeTerrainRelease(info, layers, records, payloads, derivatives->regions->writes, weights,
                                            options.pack);
        if (!encoded)
        {
            Fail(report, check::kEncode, "", encoded.error.message);
            return std::nullopt;
        }
        report.Ran(check::kEncode, std::format("release {}: {} source tiles, {} sparse layers, {} packs, {} weight "
                                               "layers over {} tiles, {} Region weight images; {}",
                                               TerrainIdHex(info.release.value), tiles.size(), layers.size(),
                                               encoded.value->packs.size(), weights.layers.size(), weightTiles.size(),
                                               regionWeightImages.size(), mode));
        return TerrainBakedRelease{info,        recipe.layout, nonce, std::move(tiles), std::move(*encoded.value),
                                   report.seams, baseRelease && affected.has_value()};
    }

    TerrainCandidateManifest MakeCandidateManifest(const TerrainBakedRelease &baked, std::string assetPath,
                                                   std::string recipePath)
    {
        TerrainCandidateManifest manifest;
        manifest.assetPath = std::move(assetPath);
        manifest.recipePath = std::move(recipePath);
        manifest.release = baked.info;
        manifest.nonce = baked.nonce;
        manifest.heightsHash = baked.encoded.heightsHash;
        const auto index = ParseTerrainIndex(baked.encoded.index);
        manifest.formatMinor = index.value->formatMinor;
        manifest.contentVersion = kTerrainHeightsContentVersion;
        manifest.surfaceVersion = kTerrainSurfaceVersion;
        manifest.layerCount = static_cast<uint32_t>(index.value->layers.size());
        manifest.payloadCount = static_cast<uint32_t>(index.value->tiles.size());
        manifest.files = DescribeReleaseFiles(baked.encoded);
        manifest.seams = baked.seams;
        return manifest;
    }

    bool BakeTerrainCandidate(const TerrainBakeRequest &request, TerrainReport &report)
    {
        fs::path out = request.candidateDir.lexically_normal();
        if (!out.empty() && !out.has_filename())
            out = out.parent_path();
        report.candidateDir = out;
        report.recipePath = request.recipePath;

        std::string assetPath;
        if (!CheckArguments(request, out, assetPath, report))
            return false;
        report.assetPath = assetPath;

        RecipeParseResult parsed = LoadTerrainRecipe(request.recipePath);
        for (RecipeIssue &issue : parsed.issues)
            report.Add(
                {std::string(check::kRecipe), std::move(issue.field), {}, std::nullopt, std::move(issue.message)});
        if (!parsed.recipe)
            return false;
        TerrainRecipe &recipe = *parsed.recipe;
        const TerrainSourceFingerprint fingerprint = FingerprintRecipe(recipe);
        report.recipeFingerprint = fingerprint;
        report.Ran(check::kRecipe, DescribeRecipeFingerprint(recipe, fingerprint));

        // Loaded here rather than at parse, so an up-to-date release needs no source file.
        if (!LoadRecipeHeightSource(recipe, report))
            return false;

        const AssetId asset = MakeAssetId(assetPath);
        std::optional<std::vector<TileHeights>> tiles =
            BakeCandidateHeights(recipe, asset, request.baseHeights, report);
        if (!tiles)
            return false;

        std::optional<TerrainReleaseHandle> baseRelease;
        std::string baseUnavailable;
        if (request.base)
        {
            if (request.base->handle)
                baseRelease = *request.base->handle;
            else if (!request.base->releasesRoot.empty() && request.base->asset)
            {
                const std::filesystem::path releases = request.base->releasesRoot;
                const AssetId baseAsset = *request.base->asset;
                TerrainResult<TerrainReleaseHandle> opened = OpenCurrentTerrainRelease(releases, baseAsset);
                if (opened)
                    baseRelease = std::move(*opened.value);
                else
                    baseUnavailable = "the base release does not open: " + opened.error.message;
            }
            else
                baseUnavailable = "the base release has no open handle or releases root and asset";
        }

        std::optional<TerrainBakedRelease> baked =
            EncodeBakedRelease(asset, fingerprint, recipe, std::move(*tiles), request.options, report,
                               baseRelease ? &*baseRelease : nullptr, std::move(baseUnavailable));
        if (!baked)
            return false;
        report.release = baked->info;
        report.heightsHash = baked->encoded.heightsHash;
        report.nonce = baked->nonce;
        report.layout = baked->layout;

        TerrainProgressStage("write");
        // Written beside the target so the rename stays on one volume.
        fs::path staging = out;
        staging += "." + TerrainIdHex(baked->info.release.value) + ".partial";
        std::error_code ec;
        if (fs::exists(staging, ec))
        {
            Fail(report, check::kWrite, "--out",
                 std::format("{} already exists (left by an interrupted bake?); remove it", PathUtf8(staging)));
            return false;
        }
        fs::create_directories(staging, ec);
        StagingCleanup cleanup{staging};
        std::string error;
        const TerrainCandidateManifest manifest =
            MakeCandidateManifest(*baked, assetPath, PathUtf8(request.recipePath));
        if (ec || !WriteCandidateFiles(staging, baked->encoded, manifest, error))
        {
            Fail(report, check::kWrite, "--out",
                 ec ? std::format("cannot create {} ({})", PathUtf8(staging), ec.message()) : error);
            return false;
        }

        TerrainProgressStage("validate");
        ValidateTerrainCandidate(
            staging,
            {&recipe, true, false, baked->incremental && baseRelease ? &*baseRelease : nullptr}, report);
        if (!report.Ok())
        {
            report.Skip(check::kWrite, std::format("the written files failed validation; {} was removed and {} "
                                                   "not created",
                                                   PathUtf8(staging), PathUtf8(out)));
            return false;
        }

        // An empty --out was allowed above; a directory rename cannot replace it.
        if (fs::exists(out, ec))
            fs::remove(out, ec);
        fs::rename(staging, out, ec);
        if (ec)
        {
            Fail(report, check::kWrite, "--out",
                 std::format("cannot rename {} to {} ({})", PathUtf8(staging), PathUtf8(out), ec.message()));
            return false;
        }
        cleanup.dir.clear();
        uint64_t bytes = 0;
        for (const TerrainFileSummary &file : manifest.files)
            bytes += file.bytes;
        report.Ran(check::kWrite, std::format("{} release files ({} bytes) and {} in {}", manifest.files.size(), bytes,
                                              kCandidateManifestFileName, PathUtf8(out)));
        return report.Ok();
    }

    TerrainReport RunTerrainBake(const TerrainBakeRequest &request)
    {
        TerrainReport report("bake", {check::kArguments, check::kRecipe, check::kGenerate, check::kEncode,
                                      check::kWrite, check::kCandidate, check::kFormat, check::kIndex, check::kPacks,
                                      check::kCoverage, check::kPayloads, check::kFinite, check::kEdges,
                                      check::kCorners, check::kRegions, check::kIdentity});
        BakeTerrainCandidate(request, report);
        return report;
    }
}
