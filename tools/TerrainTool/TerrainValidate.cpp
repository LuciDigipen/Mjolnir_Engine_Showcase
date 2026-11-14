/**
 * @file TerrainValidate.cpp
 * @author Jackey Cen
 * @brief Validates a baked terrain candidate: integrity, coverage, finite heights, seams and identity.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "TerrainValidate.h"
#include "TerrainDerivatives.h"
#include "TerrainIncremental.h"
#include "TerrainRegions.h"

#include "TerrainCandidate.h"
#include "TerrainProgress.h"
#include "TerrainSeams.h"

#include <assets/AssetId.h>
#include <core/fs/FileIO.h>
#include <terrain/TerrainPackFormat.h>
#include <terrain/TerrainPackWriter.h>
#include <terrain/TerrainRegionWeights.h>
#include <terrain/TerrainRelease.h>

#include <algorithm>
#include <format>
#include <initializer_list>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <system_error>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mir::terrainbake
{
    namespace
    {
        constexpr std::string_view kIntegrityOnlyReason =
            "publish checks integrity; bake and validate check the contents";

        std::string Tile(TileCoord t)
        {
            return std::format("({}, {})", t.x, t.z);
        }

        void SkipAll(TerrainReport &report, std::initializer_list<std::string_view> checks, const std::string &why)
        {
            for (std::string_view c : checks)
                report.Skip(c, why);
        }

        void CheckIdentity(const TerrainIndex &index, const std::optional<TerrainCandidateManifest> &manifest,
                           const TerrainRecipe *recipe, const std::vector<TerrainFileSummary> &actual,
                           TerrainReport &report)
        {
            auto mismatch = [&](std::string field, std::string what, auto expected, auto found)
            {
                report.Add({std::string(check::kIdentity),
                            std::move(field),
                            {},
                            std::nullopt,
                            std::format("{} is {}, the index has {}", what, expected, found)});
            };
            const TerrainReleaseInfo &info = index.release;
            if (manifest)
            {
                const TerrainCandidateManifest &m = *manifest;
                auto hex = [](uint64_t v)
                {
                    return TerrainIdHex(v);
                };
                if (m.release.asset != info.asset)
                    mismatch("asset.id", "candidate.json asset", hex(m.release.asset.value), hex(info.asset.value));
                if (MakeAssetId(m.assetPath) != m.release.asset)
                    report.Add({std::string(check::kIdentity),
                                "asset.logicalPath",
                                {},
                                std::nullopt,
                                std::format("candidate.json: \"{}\" hashes to asset {}, not {}", m.assetPath,
                                            hex(MakeAssetId(m.assetPath).value), hex(m.release.asset.value))});
                if (m.release.release != info.release)
                    mismatch("release.id", "candidate.json release", hex(m.release.release.value),
                             hex(info.release.value));
                if (m.release.source != info.source)
                    mismatch("recipe.fingerprint", "candidate.json source fingerprint", hex(m.release.source.value),
                             hex(info.source.value));
                if (m.layerCount != index.layers.size() || m.payloadCount != index.tiles.size())
                    mismatch("release.tables", "candidate.json layer/payload counts",
                             std::format("{}/{}", m.layerCount, m.payloadCount),
                             std::format("{}/{}", index.layers.size(), index.tiles.size()));
                if (m.heightsHash != index.heightsHash)
                    mismatch("release.heightsHash", "candidate.json heights hash", hex(m.heightsHash),
                             hex(index.heightsHash));
                if (m.release.format != info.format || m.formatMinor != index.formatMinor ||
                    m.contentVersion != index.contentVersion)
                    mismatch(
                        "release.formatVersion", "candidate.json format",
                        std::format("{}.{} content {}", m.release.format.value, m.formatMinor, m.contentVersion),
                        std::format("{}.{} content {}", info.format.value, index.formatMinor, index.contentVersion));
                const TerrainReleaseId minted =
                    MintTerrainReleaseId(m.release.asset, m.release.source, m.heightsHash, m.nonce);
                if (minted != m.release.release)
                    report.Add({std::string(check::kIdentity),
                                "release.nonce",
                                {},
                                std::nullopt,
                                std::format("candidate.json: release {} is not the id minted from its asset, "
                                            "fingerprint, heights hash and nonce ({})",
                                            hex(m.release.release.value), hex(minted.value))});

                for (const TerrainFileSummary &want : m.files)
                {
                    auto it = std::find_if(actual.begin(), actual.end(),
                                           [&](const TerrainFileSummary &f)
                                           {
                                               return f.name == want.name;
                                           });
                    if (it == actual.end())
                        report.Add(
                            {std::string(check::kIdentity),
                             "files",
                             {},
                             std::nullopt,
                             std::format("candidate.json lists {}, which is not a file of this release", want.name)});
                    else if (it->bytes != want.bytes || it->checksum != want.checksum)
                        report.Add(
                            {std::string(check::kIdentity),
                             "files",
                             {},
                             std::nullopt,
                             std::format("{} is {} bytes with Fnv1a64 {}; candidate.json recorded {} bytes "
                                         "with {} (changed after the bake)",
                                         want.name, it->bytes, hex(it->checksum), want.bytes, hex(want.checksum))});
                }
                for (const TerrainFileSummary &have : actual)
                    if (std::none_of(m.files.begin(), m.files.end(),
                                     [&](const TerrainFileSummary &f)
                                     {
                                         return f.name == have.name;
                                     }))
                        report.Add({std::string(check::kIdentity),
                                    "files",
                                    {},
                                    std::nullopt,
                                    std::format("candidate.json does not list the release file {}", have.name)});
            }
            if (recipe)
            {
                const TerrainSourceFingerprint fingerprint = FingerprintRecipe(*recipe);
                if (fingerprint != info.source)
                    report.Add({std::string(check::kIdentity),
                                "sourceFingerprint",
                                {},
                                std::nullopt,
                                std::format("the recipe's fingerprint is {}, the release was baked from {}",
                                            TerrainIdHex(fingerprint.value), TerrainIdHex(info.source.value))});
                const TerrainLayout &want = recipe->layout;
                const TerrainLayout &have = index.layout;
                auto layoutField = [&](const char *field, double expected, double found)
                {
                    if (expected != found)
                        report.Add({std::string(check::kIdentity),
                                    field,
                                    {},
                                    std::nullopt,
                                    std::format("the recipe has {}, the release {}", expected, found)});
                };
                layoutField("layout.sampleSpacing", want.SampleSpacing(), have.SampleSpacing());
                layoutField("layout.intervalsPerTile", want.IntervalsPerTile(), have.IntervalsPerTile());
                layoutField("layout.bounds.minX", want.Bounds().minX, have.Bounds().minX);
                layoutField("layout.bounds.minZ", want.Bounds().minZ, have.Bounds().minZ);
                layoutField("layout.bounds.maxX", want.Bounds().maxX, have.Bounds().maxX);
                layoutField("layout.bounds.maxZ", want.Bounds().maxZ, have.Bounds().maxZ);
                report.sourceEstimates = recipe->SourceEstimates();
                for (const auto &record : index.tiles)
                    if (record.source)
                    {
                        const auto spacing = recipe->SourceSpacing(record.tile);
                        if (!spacing || *spacing != record.sampleSpacing)
                            report.Add({std::string(check::kIdentity),
                                        "precision.tiles",
                                        {record.tile},
                                        std::nullopt,
                                        "release source spacing differs from the tile's recipe assignment"});
                    }

                auto join = [](const std::vector<std::string> &names)
                {
                    std::string text;
                    for (const std::string &name : names)
                        text += (text.empty() ? "" : ", ") + name;
                    return text;
                };
                std::vector<std::string> wantWeights;
                if (recipe->hasMaterialsSection)
                    for (const TerrainMaterialLayer &layer : recipe->materials)
                        wantWeights.push_back(layer.name);
                if (index.weightLayers != wantWeights)
                    report.Add({std::string(check::kIdentity),
                                "weight.layers",
                                {},
                                std::nullopt,
                                std::format("the recipe's material layer table is [{}], the release's is [{}]",
                                            join(wantWeights), join(index.weightLayers))});
                const uint64_t wantWeightsFingerprint =
                    wantWeights.empty() ? 0 : FingerprintWeights(*recipe, fingerprint).value;
                if (index.weightsFingerprint != wantWeightsFingerprint)
                    report.Add({std::string(check::kIdentity),
                                "weight.fingerprint",
                                {},
                                std::nullopt,
                                std::format("the recipe's weight fingerprint is {}, the release recorded {}",
                                            TerrainIdHex(wantWeightsFingerprint),
                                            TerrainIdHex(index.weightsFingerprint))});
                size_t sources = 0;
                for (const TerrainTileRecord &record : index.tiles)
                    if (record.source)
                        ++sources;
                if (!wantWeights.empty())
                {
                    if (index.weights.size() != sources)
                        report.Add({std::string(check::kIdentity),
                                    "weight.tiles",
                                    {},
                                    std::nullopt,
                                    std::format("{} source tiles need a weight payload, the release holds {}",
                                                sources, index.weights.size())});
                    for (const TerrainTileRecord &record : index.tiles)
                        if (record.source && !index.FindWeight(record.tile))
                            report.Add({std::string(check::kIdentity),
                                        "weight.tiles",
                                        {record.tile},
                                        std::nullopt,
                                        "the source tile has no weight payload"});
                }
                else if (!index.weights.empty() || !index.weightPacks.empty())
                    report.Add({std::string(check::kIdentity),
                                "weight.tiles",
                                {},
                                std::nullopt,
                                "the recipe has no materials section, the release holds weight payloads"});
            }
            if (!manifest && !recipe)
                report.Skip(check::kIdentity, "no candidate.json or recipe to compare with");
            else
                report.Ran(check::kIdentity, std::format("compared with{}{}", manifest ? " candidate.json" : "",
                                                         recipe ? (manifest ? " and the recipe" : " the recipe") : ""));
        }

        std::string Describe(TerrainRegionCoord region)
        {
            return std::format("Region L{} ({}, {})", region.level, region.x, region.z);
        }

        // Decodes every Region and rederives the hierarchy from the final sources when they all decoded.
        void CheckRegions(const TerrainReleaseHandle &handle, const std::vector<std::string> &packNames,
                          const std::vector<std::unordered_map<TileCoord, TileHeights>> &decoded,
                          const TerrainValidateOptions &options, TerrainReport &report)
        {
            const TerrainIndex &index = handle.Index();
            report.regions.clear();
            report.regionGrid.reset();
            if (!index.regionGrid)
            {
                if (options.recipe)
                    report.Add({std::string(check::kRegions),
                                "regions",
                                {},
                                std::nullopt,
                                "a recipe-backed candidate needs this tool's Region hierarchy, and the release has "
                                "none"});
                else
                    report.Skip(check::kRegions, "the release has no Region hierarchy");
                return;
            }
            const TerrainRegionGrid &grid = *index.regionGrid;
            const TerrainLayout &ref = grid.Reference();
            report.regionGrid =
                TerrainRegionGridSummary{grid.RegionTiles(), grid.Cells(), grid.Unit(), grid.Levels(),
                                         ref.SamplePosition(grid.EdgeTiles(0) * ref.IntervalsPerTile())};

            size_t readable = 0;
            uint64_t regionsDone = 0;
            for (const TerrainTileRecord &record : index.regions)
            {
                TerrainProgressStep("validate: regions", regionsDone++, index.regions.size());
                const TerrainRegionCoord region{record.level, record.tile.x, record.tile.z};
                const std::string &pack = packNames[record.pack];
                TerrainRegionSummary summary{record.level,
                                             record.tile,
                                             pack,
                                             grid.MapTiles(region),
                                             record.validExtent,
                                             record.minHeight,
                                             record.maxHeight,
                                             record.sampleSpacing,
                                             record.vertexCount,
                                             record.indexCount / 3,
                                             record.length,
                                             record.maxDeviation.value_or(0.0),
                                             false};
                if (!handle.Source().Size(pack))
                    report.Add({std::string(check::kRegions),
                                pack,
                                {},
                                std::nullopt,
                                std::format("{} is missing: its pack {} does not exist", Describe(region), pack)});
                else if (const TerrainResult<TerrainRegionMesh> mesh = handle.ReadRegion(region))
                {
                    summary.readable = true;
                    ++readable;
                }
                else
                    report.Add({std::string(check::kRegions), "region", {}, std::nullopt, mesh.error.message});
                report.regions.push_back(std::move(summary));
            }

            TerrainRecipe derivedRecipe{.layout = index.layout};
            derivedRecipe.defaultSpacing = index.layout.SampleSpacing();
            std::vector<TileHeights> sources;
            for (const TerrainTileRecord &r : index.tiles)
                if (r.source)
                {
                    derivedRecipe.sourceRules.push_back({r.tile, r.sampleSpacing});
                    const auto it = decoded[r.layer].find(r.tile);
                    if (it == decoded[r.layer].end())
                    {
                        report.Skip(check::kRegions, std::format("{} of {} Regions decode; not rederived because "
                                                                 "tile {} did not decode",
                                                                 readable, index.regions.size(), Tile(r.tile)));
                        return;
                    }
                    sources.push_back(it->second);
                }
            std::string why;
            const std::optional<TerrainRegionBake> expected =
                BakeTerrainRegions(derivedRecipe, sources, {grid.RegionTiles(), grid.Cells()}, why);
            if (!expected)
            {
                report.Add({std::string(check::kRegions), "derived", {}, std::nullopt, why});
                return;
            }
            if (!expected->writes.grid->SameGrid(grid))
            {
                report.Add({std::string(check::kRegions),
                            "derived",
                            {},
                            std::nullopt,
                            std::format("the Region grid unit is {}; the final sources give {}", grid.Unit(),
                                        expected->writes.grid->Unit())});
                return;
            }
            double worst = 0.0;
            for (size_t i = 0; i < expected->writes.payloads.size(); ++i)
            {
                const TerrainTileRecord &actual = index.regions[i];
                const TerrainResult<std::vector<std::byte>> bytes = handle.ReadPayload(actual);
                if (!bytes || *bytes.value != expected->writes.payloads[i].bytes)
                    report.Add({std::string(check::kRegions),
                                "derived",
                                {},
                                std::nullopt,
                                std::format("{} mesh, normals or error differ from the final-source derivation",
                                            Describe(grid.AtOrdinal(i)))});
                worst = std::max(worst, expected->meshes[i].maxDeviation);
            }
            report.Ran(check::kRegions,
                       std::format("{} Regions in {} levels decode and match the final-source derivation; largest "
                                   "error {} m",
                                   readable, grid.Levels(), worst));
        }

        void CheckContents(const TerrainReleaseHandle &handle, const std::vector<std::string> &packNames,
                           const TerrainValidateOptions &options, bool mixed, TerrainReport &report)
        {
            const TerrainIndex &index = handle.Index();
            const TerrainLayout &layout = index.layout;
            std::vector<std::unordered_map<TileCoord, TileHeights>> decoded(index.layers.size());
            size_t decodedCount = 0;
            size_t missing = 0;
            size_t nonFinite = 0;
            uint64_t recordsDone = 0;
            for (const TerrainTileRecord &record : index.tiles)
            {
                TerrainProgressStep("validate: tiles", recordsDone++, index.tiles.size());
                const std::string &pack = packNames[record.pack];
                const auto *sourceRecord = index.Find(record.tile);
                TerrainTileSummary summary{record.tile,
                                           pack,
                                           record.minHeight,
                                           record.maxHeight,
                                           false,
                                           sourceRecord->sampleSpacing,
                                           record.sampleSpacing,
                                           static_cast<uint32_t>(record.samplesPerSide),
                                           record.layer,
                                           record.source};
                summary.kind = static_cast<uint32_t>(record.kind);
                summary.level = record.level;
                summary.maxDeviation = record.maxDeviation;
                summary.sourceChecksum = record.sourceChecksum;
                summary.topologyHash = record.topologyHash;
                if (!handle.Source().Size(pack))
                {
                    ++missing;
                    report.Add({std::string(check::kCoverage),
                                pack,
                                {record.tile},
                                std::nullopt,
                                std::format("tile {} payload kind {} layer {} is missing: its pack {} does not exist",
                                            Tile(record.tile), summary.kind, record.layer, pack)});
                    report.tiles.push_back(summary);
                    continue;
                }
                if (record.kind != TerrainPayloadKind::Height)
                {
                    TerrainPackError error;
                    if (record.kind == TerrainPayloadKind::LodHeight)
                        error = handle.ReadLod({index.release.asset, record.tile}, record.layer).error;
                    else
                        error = handle.ReadNormals({index.release.asset, record.tile}, record.layer).error;
                    summary.readable = error.code == TerrainPackErrorCode::None;
                    if (summary.readable)
                        ++decodedCount;
                    else
                        report.Add(
                            {std::string(check::kPayloads), "derived", {record.tile}, std::nullopt, error.message});
                    report.tiles.push_back(summary);
                    continue;
                }
                if (TerrainResult<TileHeights> read = handle.ReadTileRecord(record))
                {
                    summary.readable = true;
                    decoded[record.layer].emplace(record.tile, std::move(*read.value));
                    ++decodedCount;
                }
                else if (const std::optional<GlobalSampleIndex> bad = read.error.nonFiniteSample)
                {
                    ++nonFinite;
                    report.Add({std::string(check::kFinite),
                                "samples",
                                {record.tile},
                                bad,
                                std::format("tile {} holds a non-finite height at sample ({}, {})", Tile(record.tile),
                                            bad->x, bad->z)});
                }
                else
                    report.Add(
                        {std::string(check::kPayloads), "payload", {record.tile}, std::nullopt, read.error.message});
                report.tiles.push_back(summary);
            }
            const size_t total = index.tiles.size();
            report.Ran(check::kCoverage,
                       std::format("{} of {} indexed payloads present, each key listed once", total - missing, total));
            // A tile these checks never saw keeps them from passing.
            const std::string payloads = std::format("{} of {} tiles read back and decode", decodedCount, total);
            if (missing > 0)
                report.Skip(check::kPayloads, payloads + "; missing tiles were not checked");
            else
                report.Ran(check::kPayloads, payloads);
            const size_t checkedFinite = decodedCount + nonFinite;
            const std::string finite =
                std::format("{} of {} tiles checked, {} hold non-finite heights", checkedFinite, total, nonFinite);
            if (checkedFinite < total)
                report.Skip(check::kFinite, finite);
            else
                report.Ran(check::kFinite, finite);

            for (uint32_t l = 0; l < index.layers.size(); ++l)
            {
                const auto &layerTiles = decoded[l];
                std::unordered_set<TileCoord> baked;
                for (const auto &r : index.tiles)
                    if (r.layer == l && r.kind == TerrainPayloadKind::Height)
                        baked.insert(r.tile);
                const TileSampleLookup lookup = [&](TileCoord tile) -> std::span<const float>
                {
                    const auto it = layerTiles.find(tile);
                    return it == layerTiles.end() ? std::span<const float>() : it->second.Samples();
                };
                // Sparse layers need only check pairs that were baked at that spacing.
                CheckTileContinuity(index.layers[l], lookup, report,
                                    [&baked](TileCoord tile)
                                    {
                                        return baked.contains(tile);
                                    });
            }

            if (std::any_of(index.tiles.begin(), index.tiles.end(),
                            [](const auto &r)
                            {
                                return r.kind == TerrainPayloadKind::LodHeight ||
                                       (!r.source && r.kind == TerrainPayloadKind::Height && r.sourceChecksum != 0);
                            }))
            {
                TerrainRecipe derivedRecipe{.layout = layout};
                derivedRecipe.defaultSpacing = layout.SampleSpacing();
                std::vector<TileHeights> sourceTiles;
                bool allSources = true;
                for (const auto &r : index.tiles)
                    if (r.source)
                    {
                        derivedRecipe.sourceRules.push_back({r.tile, r.sampleSpacing});
                        const auto it = decoded[r.layer].find(r.tile);
                        if (it != decoded[r.layer].end())
                            sourceTiles.push_back(it->second);
                        else
                            allSources = false;
                    }
                if (allSources)
                {
                    const TerrainReleaseHandle *base = options.baseRelease;
                    std::string ignoredBase;
                    std::unordered_set<TileCoord> affected;
                    if (base)
                    {
                        TerrainRegionSettings settings;
                        if (index.regionGrid)
                        {
                            settings.regionTiles = index.regionGrid->RegionTiles();
                            settings.cells = index.regionGrid->Cells();
                        }
                        if (base->Info().asset != index.release.asset)
                            ignoredBase = "the base release belongs to another asset";
                        else if (const std::optional<std::string> mismatch =
                                     TerrainBaseMismatch(*base, derivedRecipe, sourceTiles, settings))
                            ignoredBase = *mismatch;
                        else
                        {
                            std::size_t changed = 0;
                            const std::vector<TileCoord> tiles = TerrainAffectedTiles(*base, sourceTiles, changed);
                            affected.insert(tiles.begin(), tiles.end());
                        }
                    }
                    const bool reuseBase = base && ignoredBase.empty();
                    std::string why;
                    auto expected = BakeTerrainDerivatives(derivedRecipe, sourceTiles, why, nullptr, std::nullopt,
                                                           reuseBase ? &affected : nullptr);
                    if (!expected)
                        report.Add({std::string(check::kPayloads), "derived", {}, std::nullopt, why});
                    else if (expected->layers.size() != index.layers.size() ||
                             !std::equal(expected->layers.begin(), expected->layers.end(), index.layers.begin(),
                                         [](const auto &a, const auto &b)
                                         {
                                             return a.SampleSpacing() == b.SampleSpacing() &&
                                                    a.IntervalsPerTile() == b.IntervalsPerTile() &&
                                                    a.Bounds() == b.Bounds();
                                         }))
                        report.Add({std::string(check::kPayloads),
                                    "derivedLayers",
                                    {},
                                    std::nullopt,
                                    "derived layer coverage differs from final-source derivation"});
                    else
                    {
                        std::set<std::tuple<int32_t, int32_t, uint32_t, uint32_t, uint32_t>> expectedKeys;
                        for (const auto &v : expected->payloads)
                            expectedKeys.emplace(v.record.tile.x, v.record.tile.z, v.record.layer,
                                                 static_cast<uint32_t>(v.record.kind), v.record.level);
                        for (size_t i = 0; i < expected->queryHeights.size(); ++i)
                            expectedKeys.emplace(expected->queryHeights[i].Tile().x,
                                                 expected->queryHeights[i].Tile().z, expected->queryLayers[i],
                                                 static_cast<uint32_t>(TerrainPayloadKind::Height), 0);
                        size_t rederived = 0;
                        for (const auto &v : expected->payloads)
                        {
                            const auto &key = v.record;
                            const auto *actual = index.Find(key.tile, key.layer, key.kind, key.level);
                            auto bytes =
                                actual ? handle.ReadPayload(*actual) : TerrainResult<std::vector<std::byte>>{};
                            if (!actual || !bytes || *bytes.value != v.bytes)
                                report.Add({std::string(check::kPayloads),
                                            "derived",
                                            {key.tile},
                                            std::nullopt,
                                            "LOD topology/error or normal order/neighbourhood differs from "
                                            "final-source derivation"});
                            ++rederived;
                        }
                        for (size_t i = 0; i < expected->queryHeights.size(); ++i)
                        {
                            const auto &tile = expected->queryHeights[i];
                            const uint32_t layer = expected->queryLayers[i];
                            if (layer >= decoded.size())
                            {
                                report.Add({std::string(check::kPayloads),
                                            "queryDerivative",
                                            {tile.Tile()},
                                            std::nullopt,
                                            "query derivative layer is missing"});
                                continue;
                            }
                            const auto *actual = index.Find(tile.Tile(), layer);
                            const auto it = decoded[layer].find(tile.Tile());
                            if (!actual || actual->source || !actual->maxDeviation ||
                                *actual->maxDeviation < expected->queryErrors[i] || it == decoded[layer].end() ||
                                !std::equal(tile.Samples().begin(), tile.Samples().end(), it->second.Samples().begin(),
                                            it->second.Samples().end()))
                                report.Add({std::string(check::kPayloads),
                                            "queryDerivative",
                                            {tile.Tile()},
                                            std::nullopt,
                                            "regular query derivative/error differs from final-source derivation"});
                            ++rederived;
                        }
                        size_t reused = 0;
                        if (reuseBase)
                        {
                            for (const TerrainTileRecord &r : index.tiles)
                            {
                                if (r.source || r.kind == TerrainPayloadKind::Region || affected.contains(r.tile))
                                    continue;
                                const TerrainTileRecord *baseRecord =
                                    base->Index().Find(r.tile, r.layer, r.kind, r.level);
                                const TerrainResult<std::vector<std::byte>> baseBytes =
                                    baseRecord ? base->ReadPayload(*baseRecord)
                                               : TerrainResult<std::vector<std::byte>>{};
                                const TerrainResult<std::vector<std::byte>> bytes = handle.ReadPayload(r);
                                if (!baseRecord || !baseBytes || !bytes || *bytes.value != *baseBytes.value)
                                    report.Add({std::string(check::kPayloads),
                                                "derived",
                                                {r.tile},
                                                std::nullopt,
                                                "a reused payload differs from the base release for an unchanged "
                                                "tile"});
                                ++reused;
                            }
                            for (const TerrainTileRecord &r : index.tiles)
                            {
                                if (r.source || r.kind == TerrainPayloadKind::Region || !affected.contains(r.tile))
                                    continue;
                                if (!expectedKeys.contains({r.tile.x, r.tile.z, r.layer,
                                                            static_cast<uint32_t>(r.kind), r.level}))
                                    report.Add({std::string(check::kPayloads),
                                                "derived",
                                                {r.tile},
                                                std::nullopt,
                                                "an affected tile has a payload the derivation does not produce"});
                            }
                        }
                        const std::string note =
                            reuseBase ? std::format(", {} reused from the base release", reused)
                                      : (base ? "; the base release was ignored: " + ignoredBase
                                              : " from final source");
                        report.Ran(check::kPayloads,
                                   std::format("{} decoded payloads; {} LOD/normal/query records rederived{}",
                                               decodedCount, rederived, note));
                    }
                }
            }

            CheckRegions(handle, packNames, decoded, options, report);

            if (mixed)
            {
                const TerrainContinuityLookup sources = [&](TileCoord tile)
                {
                    const auto *record = index.Find(tile);
                    if (!record)
                        return TerrainContinuityTile{};
                    const auto it = decoded[record->layer].find(tile);
                    return TerrainContinuityTile{&index.layers[record->layer], it == decoded[record->layer].end()
                                                                                   ? std::span<const float>()
                                                                                   : it->second.Samples()};
                };
                CheckMixedTileContinuity(layout, sources, report);
            }
        }

        void CheckRegionWeights(const TerrainReleaseHandle &handle, TerrainReport &report)
        {
            const TerrainIndex &index = handle.Index();
            if (index.regionWeights.empty() || !index.regionGrid)
                return;
            std::vector<TerrainWeightTile> tiles;
            std::unordered_map<TileCoord, const TerrainWeightTile *> byTile;
            tiles.reserve(index.weights.size());
            for (const TerrainWeightRecord &record : index.weights)
            {
                TerrainResult<TerrainDecodedWeights> decoded = handle.ReadWeights(record.tile);
                if (!decoded)
                {
                    report.Add({std::string(check::kRegions),
                                "weights",
                                {record.tile},
                                std::nullopt,
                                decoded.error.message});
                    return;
                }
                tiles.push_back(ExpandTerrainWeights(*decoded.value));
            }
            for (const TerrainWeightTile &tile : tiles)
                byTile.emplace(tile.tile, &tile);
            const TerrainWeightTileLookup lookup = [&byTile](TileCoord at) -> const TerrainWeightTile *
            {
                const auto it = byTile.find(at);
                return it == byTile.end() ? nullptr : it->second;
            };

            TerrainResult<std::vector<TerrainRegionWeightImage>> expected = BuildTerrainRegionWeights(
                *index.regionGrid, index.weightLayers, lookup,
                {.side = kTerrainRegionWeightSide, .workers = std::max(1u, std::thread::hardware_concurrency())});
            if (!expected)
            {
                report.Add({std::string(check::kRegions), "weights", {}, std::nullopt, expected.error.message});
                return;
            }
            for (size_t i = 0; i < expected.value->size(); ++i)
            {
                const TerrainRegionWeightImage &image = (*expected.value)[i];
                const TerrainRegionWeightRecord &record = index.regionWeights[i];
                const std::string differs =
                    std::format("{} weight image differs from the derivation of the release's tile weights",
                                Describe(record.region));
                if (image.region != record.region || image.side != record.side || image.slots != record.slots ||
                    index.FindRegionWeight(record.region) != &record)
                {
                    report.Add({std::string(check::kRegions),
                                "weights",
                                {{record.region.x, record.region.z}},
                                std::nullopt,
                                differs});
                    return;
                }
                TerrainResult<TerrainDecodedWeights> actual = handle.ReadRegionWeights(record.region);
                if (!actual || actual.value->bytes != image.bytes)
                {
                    report.Add({std::string(check::kRegions),
                                "weights",
                                {{record.region.x, record.region.z}},
                                std::nullopt,
                                actual ? differs : actual.error.message});
                    return;
                }
            }
            if (report.Status(check::kRegions) == CheckStatus::Passed)
                report.Ran(check::kRegions, std::format("{} Region weight images match the tile-weight derivation",
                                                       index.regionWeights.size()));
        }
    }

    void ValidateTerrainCandidate(const std::filesystem::path &dir, const TerrainValidateOptions &options,
                                  TerrainReport &report)
    {
        namespace fs = std::filesystem;
        using check::kCandidate;
        report.surfaceVersion = kTerrainSurfaceVersion;

        std::error_code ec;
        if (!fs::is_directory(dir, ec))
        {
            report.Add(
                {std::string(kCandidate), "", {}, std::nullopt, std::format("{} is not a directory", PathUtf8(dir))});
            SkipAll(report,
                    {check::kFormat, check::kIndex, check::kPacks, check::kCoverage, check::kPayloads, check::kFinite,
                     check::kEdges, check::kCorners, check::kRegions, check::kIdentity},
                    "no candidate directory");
            return;
        }

        std::optional<TerrainCandidateManifest> manifest;
        const fs::path manifestPath = dir / kCandidateManifestFileName;
        if (fs::is_regular_file(manifestPath, ec))
        {
            const std::optional<std::string> text = ReadText(manifestPath);
            const ManifestParseResult parsed = text ? ParseCandidateManifest(*text) : ManifestParseResult{};
            if (!text)
                report.Add({std::string(kCandidate),
                            std::string(kCandidateManifestFileName),
                            {},
                            std::nullopt,
                            "cannot read candidate.json"});
            for (const ManifestIssue &issue : parsed.issues)
                report.Add(
                    {std::string(kCandidate), issue.field, {}, std::nullopt, "candidate.json: " + issue.message});
            manifest = parsed.manifest;
        }
        else if (options.requireManifest)
            report.Add({std::string(kCandidate),
                        std::string(kCandidateManifestFileName),
                        {},
                        std::nullopt,
                        "candidate.json is missing: a bake writes it last, so this is not a complete candidate"});
        if (manifest)
        {
            report.assetPath = manifest->assetPath;
            report.nonce = manifest->nonce;
            report.seams = manifest->seams;
            ReportSeamWarnings(report);
        }

        auto source = std::make_shared<TerrainFileSource>(dir);
        TerrainResult<TerrainReleaseHandle> opened = TerrainReleaseHandle::Open(source);
        if (!opened)
        {
            const bool unsupported = opened.error.code == TerrainPackErrorCode::Unsupported;
            report.Add({std::string(unsupported ? check::kFormat : check::kIndex),
                        std::string(kTerrainIndexFileName),
                        {},
                        std::nullopt,
                        opened.error.message});
            SkipAll(report,
                    {check::kFormat, check::kIndex, check::kPacks, check::kCoverage, check::kPayloads, check::kFinite,
                     check::kEdges, check::kCorners, check::kRegions, check::kIdentity},
                    "the release index did not open");
            report.Ran(kCandidate, manifest ? "candidate.json read" : "no candidate.json");
            return;
        }
        const TerrainReleaseHandle &handle = *opened.value;
        const TerrainIndex &index = handle.Index();
        const TerrainLayout &layout = index.layout;
        report.release = index.release;
        report.formatMinor = index.formatMinor;
        report.contentVersion = index.contentVersion;
        report.heightsHash = index.heightsHash;
        report.layout = layout;
        report.Ran(check::kIndex, std::format("{} tiles in {} packs, {} intervals per tile at {} m", index.tiles.size(),
                                              index.packs.size(), layout.IntervalsPerTile(), layout.SampleSpacing()));

        if (manifest && manifest->surfaceVersion != kTerrainSurfaceVersion)
            report.Add({std::string(check::kFormat),
                        "release.surfaceVersion",
                        {},
                        std::nullopt,
                        std::format("candidate.json: baked against surface version {}, this tool checks version {}",
                                    manifest->surfaceVersion, kTerrainSurfaceVersion)});
        report.Ran(check::kFormat,
                   std::format("format {}.{} (reader {}.{}), heights content {}, surface {}",
                               index.release.format.value, index.formatMinor, kTerrainFormatVersion.value,
                               kTerrainFormatMinor, index.contentVersion, kTerrainSurfaceVersion));

        // The index read whole, packs as the index records them.
        std::vector<std::string> packNames;
        for (const TerrainPackRecord &pack : index.packs)
            packNames.push_back(TerrainPackFileName(pack.chunk));
        std::vector<std::string> weightPackNames;
        for (const TerrainPackRecord &pack : index.weightPacks)
            weightPackNames.push_back(TerrainWeightPackFileName(pack.chunk));
        std::vector<TerrainFileSummary> files;
        if (const std::optional<std::vector<std::byte>> bytes = handle.Source().ReadAll(kTerrainIndexFileName))
            files.push_back({std::string(kTerrainIndexFileName), bytes->size(), Fnv1a64(*bytes)});
        for (size_t p = 0; p < index.packs.size(); ++p)
            files.push_back({packNames[p], index.packs[p].fileSize, index.packs[p].checksum});
        for (size_t p = 0; p < index.weightPacks.size(); ++p)
            files.push_back({weightPackNames[p], index.weightPacks[p].fileSize, index.weightPacks[p].checksum});
        report.files = files;

        std::set<std::string> known(packNames.begin(), packNames.end());
        known.insert(weightPackNames.begin(), weightPackNames.end());
        known.insert(std::string(kTerrainIndexFileName));
        known.insert(std::string(kCandidateManifestFileName));
        for (const fs::directory_entry &entry : fs::directory_iterator(dir, ec))
        {
            const std::string name = PathUtf8(entry.path().filename());
            if (!known.contains(name))
                report.Add({std::string(kCandidate),
                            name,
                            {},
                            std::nullopt,
                            std::format("{} is not part of the release (stray, or left from another bake)", name)});
        }
        report.Ran(kCandidate, manifest ? "candidate.json read; no stray files" : "no candidate.json");

        const TerrainPackError packs = handle.VerifyAllPacks();
        if (packs.code != TerrainPackErrorCode::None)
            report.Add({std::string(check::kPacks), "", {}, std::nullopt, packs.message});
        report.Ran(check::kPacks, std::format("{} packs: sizes, checksums, headers and payloads", index.packs.size()));

        bool mixed = false;
        for (const auto &record : index.tiles)
            if (record.source)
                for (const TileCoord neighbour :
                     {TileCoord{record.tile.x == INT32_MAX ? record.tile.x : record.tile.x + 1, record.tile.z},
                      TileCoord{record.tile.x, record.tile.z == INT32_MAX ? record.tile.z : record.tile.z + 1}})
                    if (const auto *other = index.Find(neighbour); other && other->layer != record.layer)
                        mixed = true;
        if (options.integrityOnly)
            SkipAll(report,
                    {check::kCoverage, check::kPayloads, check::kFinite, check::kEdges, check::kCorners,
                     check::kRegions},
                    std::string(kIntegrityOnlyReason));
        else
        {
            CheckContents(handle, packNames, options, mixed, report);
            CheckRegionWeights(handle, report);
        }
        const bool requiresSeams = (options.recipe && options.recipe->recipeVersion == 2) || mixed ||
                                   std::any_of(index.tiles.begin(), index.tiles.end(),
                                               [&](const auto &r)
                                               {
                                                   return r.source && r.sampleSpacing != layout.SampleSpacing();
                                               });
        if (manifest && requiresSeams && !manifest->seams)
            report.Add({std::string(check::kIdentity),
                        "seams",
                        {},
                        std::nullopt,
                        "v2 source candidate is missing its seam summary"});
        if (manifest && manifest->seams)
        {
            const auto &s = *manifest->seams;
            if (s.algorithmVersion != kTerrainSeamVersion || s.bandMultiplier != kTerrainSeamBandMultiplier ||
                s.weight != "smoothstep" || s.warningThreshold != kTerrainSeamWarningThreshold)
                report.Add({std::string(check::kIdentity),
                            "seams",
                            {},
                            std::nullopt,
                            "candidate seam algorithm differs from this tool"});
            if (s.adjustments.size() != static_cast<size_t>(std::count_if(index.tiles.begin(), index.tiles.end(),
                                                                          [](const auto &r)
                                                                          {
                                                                              return r.source;
                                                                          })))
                report.Add({std::string(check::kIdentity),
                            "seams.adjustments",
                            {},
                            std::nullopt,
                            "seam summary does not cover each source tile"});
            for (const auto &a : s.adjustments)
            {
                const auto *record = index.Find(a.tile);
                auto fail = [&](const char *message)
                {
                    report.Add({std::string(check::kIdentity), "seams.adjustments", {a.tile}, std::nullopt, message});
                };
                if (!record)
                {
                    fail("adjustment names a tile outside the source index");
                    continue;
                }
                for (TileCoord t : a.relatedTiles)
                    if (!index.Find(t) || t == a.tile || std::abs(int64_t(t.x) - a.tile.x) > 1 ||
                        std::abs(int64_t(t.z) - a.tile.z) > 1)
                        fail("related tile is not an incident source neighbour");
                if (a.maxAdjustment == 0.0)
                {
                    if (!a.border.empty() || a.bandWidth != 0.0 || !a.relatedTiles.empty())
                        fail("zero adjustment must have no border or band");
                    continue;
                }
                const TerrainLayout &sourceLayout = index.layers[record->layer];
                const int64_t firstX = sourceLayout.TileFirstSample(a.tile.x),
                              firstZ = sourceLayout.TileFirstSample(a.tile.z);
                const int64_t n = sourceLayout.IntervalsPerTile();
                auto onGrid = [&](double coordinate, int64_t first)
                {
                    if (coordinate < sourceLayout.SamplePosition(first) ||
                        coordinate > sourceLayout.SamplePosition(first + n))
                        return false;
                    const int64_t local =
                        std::llround((coordinate - sourceLayout.SamplePosition(first)) / sourceLayout.SampleSpacing());
                    return local >= 0 && local <= n && coordinate == sourceLayout.SamplePosition(first + local);
                };
                if (!onGrid(a.x, firstX) || !onGrid(a.z, firstZ))
                    fail("adjustment position is not a source sample of its tile");
                TileCoord neighbour = a.tile;
                if (a.border == "left")
                    --neighbour.x;
                else if (a.border == "right")
                    ++neighbour.x;
                else if (a.border == "bottom")
                    --neighbour.z;
                else if (a.border == "top")
                    ++neighbour.z;
                else
                {
                    fail("positive adjustment has no valid contributing border");
                    continue;
                }
                const auto *other = index.Find(neighbour);
                const double edgeSpacing =
                    std::max(record->sampleSpacing, other ? other->sampleSpacing : record->sampleSpacing);
                const double expectedWidth =
                    std::min(layout.TileSize() * 0.5, kTerrainSeamBandMultiplier * edgeSpacing);
                if (a.bandWidth != expectedWidth)
                    fail("adjustment band width differs from its official edge band");
                const double distance = a.border == "left"     ? a.x - sourceLayout.SamplePosition(firstX)
                                        : a.border == "right"  ? sourceLayout.SamplePosition(firstX + n) - a.x
                                        : a.border == "bottom" ? a.z - sourceLayout.SamplePosition(firstZ)
                                                               : sourceLayout.SamplePosition(firstZ + n) - a.z;
                if (distance < 0.0 || distance >= expectedWidth)
                    fail("positive adjustment position lies outside its active contributing band");
            }
        }
        CheckIdentity(index, manifest, options.recipe, files, report);
    }

    TerrainReport RunTerrainValidate(const TerrainValidateRequest &request)
    {
        TerrainReport report("validate",
                             {check::kArguments, check::kRecipe, check::kCandidate, check::kFormat, check::kIndex,
                              check::kPacks, check::kCoverage, check::kPayloads, check::kFinite, check::kEdges,
                              check::kCorners, check::kRegions, check::kIdentity});
        report.candidateDir = request.candidateDir;
        report.recipePath = request.recipePath;
        if (request.candidateDir.empty())
        {
            report.Add({std::string(check::kArguments), "--candidate", {}, std::nullopt, "no candidate directory"});
            return report;
        }
        report.Ran(check::kArguments, "candidate " + PathUtf8(request.candidateDir));

        std::optional<TerrainRecipe> recipe;
        if (request.recipePath.empty())
            report.Skip(check::kRecipe, "no --recipe given");
        else
        {
            RecipeParseResult parsed = LoadTerrainRecipe(request.recipePath);
            for (RecipeIssue &issue : parsed.issues)
                report.Add(
                    {std::string(check::kRecipe), std::move(issue.field), {}, std::nullopt, std::move(issue.message)});
            if (parsed.recipe)
            {
                recipe = std::move(parsed.recipe);
                report.recipeFingerprint = FingerprintRecipe(*recipe);
            }
            report.Ran(check::kRecipe, recipe ? DescribeRecipeFingerprint(*recipe, *report.recipeFingerprint)
                                              : std::string("invalid"));
        }

        ValidateTerrainCandidate(request.candidateDir,
                                 {recipe ? &*recipe : nullptr, true, false,
                                  request.baseRelease ? &*request.baseRelease : nullptr},
                                 report);
        return report;
    }
}
