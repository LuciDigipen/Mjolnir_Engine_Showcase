/**
 * @file TerrainOverlayMesh.tests.cpp
 * @author Jackey Cen
 * @brief Tests for terrain/TerrainOverlayMesh.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "TerrainBakeTestSupport.h"

#include <terrain/TerrainEditLayer.h>
#include <terrain/TerrainLod.h>
#include <terrain/TerrainOverlay.h>
#include <terrain/TerrainOverlayMesh.h>
#include <terrain/TerrainRelease.h>
#include <terrain/TerrainTileMesh.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace mir;
using namespace mir::terrainbake;
using namespace mir::terrainbake::test;

namespace
{
    TerrainBakeRequest Request(const fs::path &recipe, std::string_view sourcePath, const fs::path &out)
    {
        TerrainBakeRequest request;
        request.recipePath = recipe;
        request.idRoot = std::string(kIdRoot);
        request.sourcePath = std::string(sourcePath);
        request.candidateDir = out;
        request.options.nonce = kNonce;
        return request;
    }

    TerrainReleaseHandle Open(const fs::path &dir)
    {
        TerrainResult<TerrainReleaseHandle> opened = TerrainReleaseHandle::Open(std::make_shared<TerrainFileSource>(dir));
        INFO(opened.error.message);
        REQUIRE(opened);
        return std::move(*opened.value);
    }

    // Every tile whose source is on the release's reference spacing, in (z, x) order.
    std::vector<TileCoord> ReferenceTiles(const TerrainIndex &index)
    {
        std::vector<TileCoord> tiles;
        for (const TerrainTileRecord &record : index.tiles)
            if (record.source && record.layer == 0)
                tiles.push_back(record.tile);
        return tiles;
    }

    // The bake only takes edit samples a reference-spacing tile owns: a coarser neighbour owns the fine tile's
    // inner edges, and its min-x row is padding.
    TerrainEditLayer EditLayer(const TerrainLayout &layout, TileCoord tile, bool everyTileReference)
    {
        const int32_t n = layout.IntervalsPerTile();
        const int64_t firstX = layout.TileFirstSample(tile.x), firstZ = layout.TileFirstSample(tile.z);
        TerrainEditLayer layer = AdditiveEditLayer("overlay check");
        SetTerrainEditSample(layer, layout, {firstX + 5, firstZ + 7}, 3.5f);
        if (everyTileReference)
        {
            SetTerrainEditSample(layer, layout, {firstX + n, firstZ + 4}, 2.25f);
            SetTerrainEditSample(layer, layout, {firstX + n, firstZ + n}, 4.5f);
        }
        else
            SetTerrainEditSample(layer, layout, {firstX + 8, firstZ}, 2.25f);
        return layer;
    }
}

TEST_CASE("An overlay mesh repeats the edited bake's level-0 positions, indices and normals", "[terraintool][overlay]")
{
    const std::pair<std::string_view, std::string_view> fixtures[]{
        {"relief-2x2.terrain.json", "terrain/relief-2x2.terrain.json"},
        {"mixed-2x2.terrain.json", "terrain/mixed-2x2.terrain.json"}};

    for (const auto &[file, sourcePath] : fixtures)
    {
        INFO(file);
        ScratchDir scratch("overlay_mesh");
        const fs::path plainRecipe = CopyRecipeFixture(std::string(file), scratch.path / "plain");
        const fs::path editedRecipe = CopyRecipeFixture(std::string(file), scratch.path / "edited");
        const RecipeParseResult parsed = LoadTerrainRecipe(editedRecipe);
        REQUIRE(parsed.recipe);
        const TerrainLayout &layout = parsed.recipe->layout;

        REQUIRE(RunTerrainBake(Request(plainRecipe, sourcePath, scratch.path / "plain" / "candidate")).Ok());
        const TerrainReleaseHandle plain = Open(scratch.path / "plain" / "candidate");
        const std::vector<TileCoord> reference = ReferenceTiles(plain.Index());
        REQUIRE_FALSE(reference.empty());
        const size_t sources = size_t(std::count_if(plain.Index().tiles.begin(), plain.Index().tiles.end(),
                                                    [](const TerrainTileRecord &record) { return record.source; }));
        const TerrainEditLayer layer = EditLayer(layout, reference.front(), reference.size() == sources);
        WriteEditLayers(editedRecipe, std::span(&layer, 1));

        const TerrainReport editedReport =
            RunTerrainBake(Request(editedRecipe, sourcePath, scratch.path / "edited" / "candidate"));
        for (const TerrainDiagnostic &diagnostic : editedReport.diagnostics)
            UNSCOPED_INFO(FormatDiagnostic(diagnostic));
        REQUIRE(editedReport.Ok());
        const TerrainReleaseHandle edited = Open(scratch.path / "edited" / "candidate");
        REQUIRE(plain.Info().asset == edited.Info().asset);
        REQUIRE(plain.Info().release != edited.Info().release);

        std::unordered_map<TileCoord, std::shared_ptr<const TileHeights>> held;
        for (const TileCoord tile : reference)
        {
            const TerrainTileRecord *record = edited.Index().Find(tile);
            REQUIRE(record);
            TerrainResult<TileHeights> heights = edited.ReadTileRecord(*record);
            REQUIRE(heights);
            held.emplace(tile, std::make_shared<const TileHeights>(std::move(*heights.value)));
        }
        const TerrainOverlay overlay(plain.Info().asset, 1, std::move(held));

        for (const TileCoord tile : reference)
        {
            INFO("tile " << tile.x << ", " << tile.z);
            const TerrainTileRecord *source = plain.Index().Find(tile);
            REQUIRE(source);
            const uint32_t layer = source->layer;
            TerrainResult<TerrainLodSurface> topology = plain.ReadLod({plain.Info().asset, tile}, layer);
            REQUIRE(topology);
            TerrainResult<TerrainLodSurface> expected = edited.ReadLod({edited.Info().asset, tile}, layer);
            REQUIRE(expected);
            TerrainResult<TerrainLodNormals> expectedNormals = edited.ReadNormals({edited.Info().asset, tile}, layer);
            REQUIRE(expectedNormals);

            std::vector<std::shared_ptr<const TileHeights>> sources;
            for (const TileCoord other : TerrainOverlaySourceTiles(plain.Index(), tile))
            {
                TerrainResult<TileHeights> heights = plain.ReadTileRecord(*plain.Index().Find(other));
                REQUIRE(heights);
                sources.push_back(std::make_shared<const TileHeights>(std::move(*heights.value)));
            }

            TerrainResult<TerrainTileMesh> built = BuildTerrainOverlayTileMesh(
                plain.Index(), plain.Info(), tile, *topology.value, overlay, sources);
            REQUIRE(built);
            const TerrainTileMesh &mesh = *built.value;
            REQUIRE(mesh.vertices.size() == expected.value->positions.size());
            REQUIRE(mesh.indices == expected.value->indices);
            if (tile == reference.front())
            {
                bool edited = false;
                for (size_t i = 0; i < mesh.vertices.size(); ++i)
                    edited = edited || mesh.vertices[i].position.y != topology.value->positions[i].y;
                CHECK(edited);
            }
            for (size_t i = 0; i < mesh.vertices.size(); ++i)
            {
                const glm::vec3 &position = mesh.vertices[i].position;
                const glm::vec3 &want = expected.value->positions[i];
                CHECK(std::bit_cast<uint32_t>(position.x) == std::bit_cast<uint32_t>(want.x));
                CHECK(std::bit_cast<uint32_t>(position.y) == std::bit_cast<uint32_t>(want.y));
                CHECK(std::bit_cast<uint32_t>(position.z) == std::bit_cast<uint32_t>(want.z));
                const glm::vec3 &normal = mesh.vertices[i].normal;
                const glm::vec3 &wantNormal = expectedNormals.value->normals[i];
                CHECK(std::bit_cast<uint32_t>(normal.x) == std::bit_cast<uint32_t>(wantNormal.x));
                CHECK(std::bit_cast<uint32_t>(normal.y) == std::bit_cast<uint32_t>(wantNormal.y));
                CHECK(std::bit_cast<uint32_t>(normal.z) == std::bit_cast<uint32_t>(wantNormal.z));
            }
        }
    }
}
