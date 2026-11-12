/**
 * @file TerrainEditLayer.tests.cpp
 * @author Jackey Cen
 * @brief Tests for terrain/TerrainEditLayer.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include <terrain/TerrainEditLayer.h>

#include <assets/AssetId.h>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace mir;

namespace
{
    using Code = TerrainPackErrorCode;
    using namespace terraineditformat;

    constexpr size_t kPayloadOffset = kHeaderSize + kChannelSize;

    TerrainLayout EditLayout()
    {
        const auto layout = TerrainLayout::Create(1.0, 4, {0.0, 0.0, 8.0, 8.0});
        REQUIRE(layout);
        return *layout;
    }

    float Analytic(int64_t i, int64_t k)
    {
        return static_cast<float>(i * 3 - k * 5) * 0.25f + static_cast<float>((i * k) % 7) * 0.125f;
    }

    std::vector<TileHeights> BaseTiles(const TerrainLayout &layout)
    {
        std::vector<TileHeights> tiles;
        const TileRange range = layout.Tiles();
        const int32_t intervals = layout.IntervalsPerTile();
        for (int32_t z = range.min.z; z <= range.max.z; ++z)
            for (int32_t x = range.min.x; x <= range.max.x; ++x)
            {
                std::optional<TileHeights> tile = TileHeights::Create({x, z}, intervals);
                REQUIRE(tile);
                for (int32_t k = 0; k <= intervals; ++k)
                    for (int32_t i = 0; i <= intervals; ++i)
                        tile->Set(i, k, Analytic(layout.TileFirstSample(x) + i, layout.TileFirstSample(z) + k));
                tiles.push_back(std::move(*tile));
            }
        return tiles;
    }

    TileHeights BaseTile(TileCoord coord, int32_t intervals, float value = 0.0f)
    {
        std::optional<TileHeights> tile = TileHeights::Create(coord, intervals);
        REQUIRE(tile);
        for (float &sample : tile->Samples())
            sample = value;
        return std::move(*tile);
    }

    TerrainEditTile FilledTile(TileCoord coord, int32_t intervals, float value,
                               std::optional<float> alpha = std::nullopt)
    {
        const size_t count = static_cast<size_t>(intervals + 1) * static_cast<size_t>(intervals + 1);
        TerrainEditTile tile;
        tile.tile = coord;
        tile.intervals = intervals;
        tile.values.assign(count, value);
        if (alpha)
            tile.alpha.assign(count, *alpha);
        return tile;
    }

    TerrainEditLayer Layer(std::string name, int32_t priority, uint32_t sequence, TerrainEditBlend blend,
                           bool enabled = true)
    {
        TerrainEditLayer layer;
        layer.info = {std::move(name), priority, sequence, enabled, blend};
        return layer;
    }

    const TerrainEditTile *FindTile(const TerrainEditLayer &layer, TileCoord coord)
    {
        const auto found = layer.tiles.find(coord);
        return found == layer.tiles.end() ? nullptr : &found->second;
    }

    float ValueAt(const TerrainEditTile &tile, GlobalSampleIndex sample, int32_t intervals)
    {
        const int64_t localX = sample.x - static_cast<int64_t>(tile.tile.x) * intervals;
        const int64_t localZ = sample.z - static_cast<int64_t>(tile.tile.z) * intervals;
        return tile.values[static_cast<size_t>(localZ) * static_cast<size_t>(intervals + 1) +
                           static_cast<size_t>(localX)];
    }

    bool SameBits(float left, float right)
    {
        return std::bit_cast<uint32_t>(left) == std::bit_cast<uint32_t>(right);
    }

    bool Mentions(const TerrainPackError &error, std::string_view text)
    {
        UNSCOPED_INFO(error.message);
        return error.message.find(text) != std::string::npos;
    }

    void StoreU32(std::vector<std::byte> &bytes, size_t at, uint32_t value)
    {
        for (int i = 0; i < 4; ++i)
            bytes[at + i] = static_cast<std::byte>((value >> (8 * i)) & 0xFFu);
    }

    void StoreF32(std::vector<std::byte> &bytes, size_t at, float value)
    {
        StoreU32(bytes, at, std::bit_cast<uint32_t>(value));
    }

    void Reseal(std::vector<std::byte> &bytes)
    {
        const size_t body = bytes.size() - kTrailerSize;
        const uint64_t checksum = Fnv1a64(std::span<const std::byte>(bytes).first(body));
        for (int i = 0; i < 8; ++i)
            bytes[body + i] = static_cast<std::byte>((checksum >> (8 * i)) & 0xFFu);
    }
} // namespace

TEST_CASE("An additive layer raises the edited tile and leaves other tiles bit-identical", "[terrain][edit]")
{
    const int32_t intervals = 4;
    TerrainEditLayer layer = Layer("raise", 0, 0, TerrainEditBlend::Additive);
    layer.tiles.emplace(TileCoord{0, 0}, FilledTile({0, 0}, intervals, 0.5f));

    const TerrainResult<TileHeights> edited =
        CompositeTerrainEditLayers(BaseTile({0, 0}, intervals, 2.0f), std::span(&layer, 1));
    REQUIRE(edited);
    CHECK(edited.value->At(2, 3) == 2.5f);

    const TileHeights absentBase = BaseTile({0, 1}, intervals, 2.0f);
    const TerrainResult<TileHeights> absent = CompositeTerrainEditLayers(absentBase, std::span(&layer, 1));
    REQUIRE(absent);
    for (size_t index = 0; index < absentBase.Samples().size(); ++index)
        CHECK(SameBits(absent.value->Samples()[index], absentBase.Samples()[index]));
}

TEST_CASE("An alpha blend layer interpolates toward its value", "[terrain][edit]")
{
    TerrainEditLayer layer = Layer("blend", 0, 0, TerrainEditBlend::AlphaBlend);
    layer.tiles.emplace(TileCoord{0, 0}, FilledTile({0, 0}, 2, 6.0f, 0.25f));

    const TerrainResult<TileHeights> edited =
        CompositeTerrainEditLayers(BaseTile({0, 0}, 2, 2.0f), std::span(&layer, 1));
    REQUIRE(edited);
    for (const float sample : edited.value->Samples())
        CHECK(sample == 3.0f);
}

TEST_CASE("A zero edit keeps a negative-zero shared edge bit-identical", "[terrain][edit]")
{
    const int32_t intervals = 4;
    const TileCoord leftCoord{0, 0};
    const TileCoord rightCoord{1, 0};
    const size_t interior = static_cast<size_t>(intervals + 1) + 1;

    for (const TerrainEditBlend blend : {TerrainEditBlend::Additive, TerrainEditBlend::AlphaBlend})
    {
        TerrainEditLayer layer = Layer("zero", 0, 0, blend);
        TerrainEditTile tile = FilledTile(leftCoord, intervals, 0.0f);
        if (blend == TerrainEditBlend::AlphaBlend)
            tile.alpha.assign(tile.values.size(), 0.0f);
        tile.values[interior] = 1.5f;
        if (!tile.alpha.empty())
            tile.alpha[interior] = 0.5f;
        layer.tiles.emplace(leftCoord, std::move(tile));

        const std::array<TileHeights, 2> bases{BaseTile(leftCoord, intervals, -0.0f),
                                              BaseTile(rightCoord, intervals, -0.0f)};
        const TerrainResult<TileHeights> left = CompositeTerrainEditLayers(bases[0], std::span(&layer, 1));
        const TerrainResult<TileHeights> right = CompositeTerrainEditLayers(bases[1], std::span(&layer, 1));
        REQUIRE(left);
        REQUIRE(right);
        for (int32_t k = 0; k <= intervals; ++k)
        {
            CHECK(SameBits(left.value->At(intervals, k), right.value->At(0, k)));
            CHECK(std::bit_cast<uint32_t>(left.value->At(intervals, k)) == 0x80000000u);
        }
        CHECK(left.value->At(1, 1) != 0.0f);
    }
}

TEST_CASE("A mixed stack applies layers in priority then sequence order", "[terrain][edit]")
{
    const auto compose = [](std::span<const TerrainEditLayer> layers) {
        const TerrainResult<TileHeights> result = CompositeTerrainEditLayers(BaseTile({0, 0}, 1, 0.0f), layers);
        REQUIRE(result);
        return result.value->At(0, 0);
    };

    TerrainEditLayer additive = Layer("add", 0, 1, TerrainEditBlend::Additive);
    additive.tiles.emplace(TileCoord{0, 0}, FilledTile({0, 0}, 1, 10.0f));
    TerrainEditLayer alpha = Layer("alpha", 0, 0, TerrainEditBlend::AlphaBlend);
    alpha.tiles.emplace(TileCoord{0, 0}, FilledTile({0, 0}, 1, 0.0f, 0.5f));

    std::array<TerrainEditLayer, 2> sequenceOrder{additive, alpha};
    CHECK(compose(sequenceOrder) == 10.0f);

    std::array<TerrainEditLayer, 2> swappedSequence{additive, alpha};
    swappedSequence[0].info.sequence = 0;
    swappedSequence[1].info.sequence = 1;
    CHECK(compose(swappedSequence) == 5.0f);

    std::array<TerrainEditLayer, 2> priorityOrder{additive, alpha};
    priorityOrder[0].info.priority = 2;
    priorityOrder[0].info.sequence = 0;
    priorityOrder[1].info.priority = 1;
    priorityOrder[1].info.sequence = 9;
    CHECK(compose(priorityOrder) == 10.0f);
}

TEST_CASE("A disabled layer contributes nothing", "[terrain][edit]")
{
    TerrainEditLayer layer = Layer("off", 0, 0, TerrainEditBlend::Additive, false);
    layer.tiles.emplace(TileCoord{0, 0}, FilledTile({0, 0}, 2, 100.0f));

    const TileHeights base = BaseTile({0, 0}, 2, 1.5f);
    const TerrainResult<TileHeights> result = CompositeTerrainEditLayers(base, std::span(&layer, 1));
    REQUIRE(result);
    for (size_t index = 0; index < base.Samples().size(); ++index)
        CHECK(SameBits(result.value->Samples()[index], base.Samples()[index]));
}

TEST_CASE("Compositing refuses an intervals mismatch and a non-finite result", "[terrain][edit]")
{
    TerrainEditLayer mismatch = Layer("mismatch", 0, 0, TerrainEditBlend::Additive);
    mismatch.tiles.emplace(TileCoord{0, 0}, FilledTile({0, 0}, 2, 1.0f));
    const TerrainResult<TileHeights> mismatchResult =
        CompositeTerrainEditLayers(BaseTile({0, 0}, 4), std::span(&mismatch, 1));
    CHECK_FALSE(mismatchResult);
    CHECK(Mentions(mismatchResult.error, "mismatch"));
    CHECK(Mentions(mismatchResult.error, "intervals"));

    TerrainEditLayer overflow = Layer("overflow", 0, 0, TerrainEditBlend::Additive);
    TerrainEditTile tile = FilledTile({0, 0}, 2, 0.0f);
    tile.values[3] = std::numeric_limits<float>::infinity();
    overflow.tiles.emplace(TileCoord{0, 0}, std::move(tile));
    const TerrainResult<TileHeights> overflowResult =
        CompositeTerrainEditLayers(BaseTile({0, 0}, 2), std::span(&overflow, 1));
    CHECK_FALSE(overflowResult);
    CHECK(Mentions(overflowResult.error, "overflow"));
    CHECK(Mentions(overflowResult.error, "sample 3"));
}

TEST_CASE("Writing a shared sample reaches every tile that holds it", "[terrain][edit]")
{
    const TerrainLayout layout = EditLayout();
    TerrainEditLayer layer = Layer("paint", 0, 0, TerrainEditBlend::Additive);

    SetTerrainEditSample(layer, layout, {4, 2}, 3.0f);
    CHECK(layer.tiles.size() == 2);
    REQUIRE(FindTile(layer, {0, 0}));
    REQUIRE(FindTile(layer, {1, 0}));
    CHECK(ValueAt(*FindTile(layer, {0, 0}), {4, 2}, layout.IntervalsPerTile()) == 3.0f);
    CHECK(ValueAt(*FindTile(layer, {1, 0}), {4, 2}, layout.IntervalsPerTile()) == 3.0f);

    SetTerrainEditSample(layer, layout, {4, 4}, 7.0f);
    CHECK(layer.tiles.size() == 4);
    for (const TileCoord coord : {TileCoord{0, 0}, TileCoord{1, 0}, TileCoord{0, 1}, TileCoord{1, 1}})
    {
        REQUIRE(FindTile(layer, coord));
        CHECK(ValueAt(*FindTile(layer, coord), {4, 4}, layout.IntervalsPerTile()) == 7.0f);
    }

    SetTerrainEditSample(layer, layout, {8, 2}, 5.0f);
    CHECK(layer.tiles.size() == 4);
    CHECK(FindTile(layer, {2, 0}) == nullptr);
    CHECK(ValueAt(*FindTile(layer, {1, 0}), {8, 2}, layout.IntervalsPerTile()) == 5.0f);
    CHECK(ValidateTerrainEditSharedSamples(layer, layout).empty());
}

TEST_CASE("Composites of adjacent tiles agree bit for bit after shared writes", "[terrain][edit]")
{
    const TerrainLayout layout = EditLayout();
    const int32_t intervals = layout.IntervalsPerTile();
    TerrainEditLayer layer = Layer("paint", 0, 0, TerrainEditBlend::Additive);
    SetTerrainEditSample(layer, layout, {4, 2}, 3.0f);
    SetTerrainEditSample(layer, layout, {4, 4}, 7.0f);

    const std::vector<TileHeights> bases = BaseTiles(layout);
    const auto compositeOf = [&](TileCoord coord) {
        for (const TileHeights &base : bases)
            if (base.Tile() == coord)
            {
                const TerrainResult<TileHeights> result = CompositeTerrainEditLayers(base, std::span(&layer, 1));
                REQUIRE(result);
                return *result.value;
            }
        REQUIRE(false);
        return TileHeights::Create(coord, intervals).value();
    };

    const TileHeights left = compositeOf({0, 0});
    const TileHeights right = compositeOf({1, 0});
    const TileHeights leftBelow = compositeOf({0, 1});
    const TileHeights rightBelow = compositeOf({1, 1});
    for (int32_t k = 0; k <= intervals; ++k)
    {
        CHECK(SameBits(left.At(intervals, k), right.At(0, k)));
        CHECK(SameBits(leftBelow.At(intervals, k), rightBelow.At(0, k)));
    }
    for (int32_t i = 0; i <= intervals; ++i)
    {
        CHECK(SameBits(left.At(i, intervals), leftBelow.At(i, 0)));
        CHECK(SameBits(right.At(i, intervals), rightBelow.At(i, 0)));
    }
    CHECK(SameBits(left.At(intervals, intervals), rightBelow.At(0, 0)));
}

TEST_CASE("The shared sample validator reports a hand-made mismatch", "[terrain][edit]")
{
    const TerrainLayout layout = EditLayout();
    TerrainEditLayer layer = Layer("paint", 0, 0, TerrainEditBlend::Additive);
    SetTerrainEditSample(layer, layout, {4, 2}, 3.0f);

    TerrainEditTile &right = layer.tiles.at({1, 0});
    right.values[static_cast<size_t>(2) * static_cast<size_t>(layout.IntervalsPerTile() + 1)] = 9.0f;

    const std::vector<GlobalSampleIndex> mismatches = ValidateTerrainEditSharedSamples(layer, layout);
    REQUIRE(mismatches.size() == 1);
    CHECK(mismatches[0] == GlobalSampleIndex{4, 2});
}

TEST_CASE("The shared sample validator reports a stored edge beside an absent neighbour", "[terrain][edit]")
{
    const TerrainLayout layout = EditLayout();
    const int32_t intervals = layout.IntervalsPerTile();
    TerrainEditLayer layer = Layer("single", 0, 0, TerrainEditBlend::Additive);
    TerrainEditTile tile = FilledTile({0, 0}, intervals, 0.0f);
    tile.values[static_cast<size_t>(2) * static_cast<size_t>(intervals + 1) + static_cast<size_t>(intervals)] = 4.0f;
    layer.tiles.emplace(TileCoord{0, 0}, std::move(tile));

    const std::vector<GlobalSampleIndex> mismatches = ValidateTerrainEditSharedSamples(layer, layout);
    REQUIRE(mismatches.size() == 1);
    CHECK(mismatches[0] == GlobalSampleIndex{intervals, 2});
}

TEST_CASE("A layer tile encodes and decodes bit exactly", "[terrain][edit]")
{
    TerrainEditTile tile = FilledTile({-3, 7}, 2, 0.0f, 0.25f);
    for (size_t index = 0; index < tile.values.size(); ++index)
    {
        tile.values[index] = -1.25f + static_cast<float>(index) * 0.5f;
        tile.alpha[index] = static_cast<float>(index) / 8.0f;
    }

    const TerrainResult<std::vector<std::byte>> encoded = EncodeTerrainEditTile(tile, TerrainEditBlend::AlphaBlend);
    REQUIRE(encoded);
    const TerrainResult<TerrainEditDecodedTile> decoded = DecodeTerrainEditTile(std::span<const std::byte>(*encoded.value));
    REQUIRE(decoded);
    CHECK(decoded.value->blend == TerrainEditBlend::AlphaBlend);
    CHECK(decoded.value->tile.tile == tile.tile);
    CHECK(decoded.value->tile.intervals == tile.intervals);
    REQUIRE(decoded.value->tile.values.size() == tile.values.size());
    REQUIRE(decoded.value->tile.alpha.size() == tile.alpha.size());
    for (size_t index = 0; index < tile.values.size(); ++index)
    {
        CHECK(SameBits(decoded.value->tile.values[index], tile.values[index]));
        CHECK(SameBits(decoded.value->tile.alpha[index], tile.alpha[index]));
    }

    TerrainEditTile additive = FilledTile({0, 0}, 1, 4.5f);
    const TerrainResult<std::vector<std::byte>> additiveEncoded =
        EncodeTerrainEditTile(additive, TerrainEditBlend::Additive);
    REQUIRE(additiveEncoded);
    const TerrainResult<TerrainEditDecodedTile> additiveDecoded =
        DecodeTerrainEditTile(std::span<const std::byte>(*additiveEncoded.value));
    REQUIRE(additiveDecoded);
    CHECK(additiveDecoded.value->blend == TerrainEditBlend::Additive);
    CHECK(additiveDecoded.value->tile.alpha.empty());
    CHECK(additiveDecoded.value->tile == additive);
}

TEST_CASE("A layer tile blob rejects every malformed input", "[terrain][edit]")
{
    const TerrainEditTile tile = FilledTile({1, 2}, 2, 1.5f, 0.5f);
    const TerrainResult<std::vector<std::byte>> encoded = EncodeTerrainEditTile(tile, TerrainEditBlend::AlphaBlend);
    REQUIRE(encoded);
    const std::vector<std::byte> good = *encoded.value;
    const size_t samples = tile.values.size();
    const size_t alphaOffset = kPayloadOffset + samples * sizeof(float);

    const auto decode = [](const std::vector<std::byte> &bytes) {
        return DecodeTerrainEditTile(std::span<const std::byte>(bytes));
    };

    SECTION("truncated blob")
    {
        std::vector<std::byte> bad = good;
        bad.pop_back();
        const TerrainResult<TerrainEditDecodedTile> result = decode(bad);
        CHECK_FALSE(result);
        CHECK(result.error.code == Code::Malformed);
    }

    SECTION("wrong magic")
    {
        std::vector<std::byte> bad = good;
        StoreU32(bad, kMagicOffset, 0);
        const TerrainResult<TerrainEditDecodedTile> result = decode(bad);
        CHECK_FALSE(result);
        CHECK(result.error.code == Code::Malformed);
    }

    SECTION("unsupported version")
    {
        std::vector<std::byte> bad = good;
        StoreU32(bad, kVersionOffset, 2);
        const TerrainResult<TerrainEditDecodedTile> result = decode(bad);
        CHECK_FALSE(result);
        CHECK(result.error.code == Code::Unsupported);
    }

    SECTION("intervals above the limit")
    {
        std::vector<std::byte> bad = good;
        StoreU32(bad, kIntervalsOffset, static_cast<uint32_t>(kMaxTerrainTileIntervals) + 1);
        const TerrainResult<TerrainEditDecodedTile> result = decode(bad);
        CHECK_FALSE(result);
        CHECK(result.error.code == Code::Malformed);
    }

    SECTION("payload size disagrees with intervals")
    {
        std::vector<std::byte> bad = good;
        StoreU32(bad, kIntervalsOffset, 3);
        const TerrainResult<TerrainEditDecodedTile> result = decode(bad);
        CHECK_FALSE(result);
        CHECK(result.error.code == Code::Malformed);
    }

    SECTION("NaN sample")
    {
        std::vector<std::byte> bad = good;
        StoreF32(bad, kPayloadOffset, std::numeric_limits<float>::quiet_NaN());
        Reseal(bad);
        const TerrainResult<TerrainEditDecodedTile> result = decode(bad);
        CHECK_FALSE(result);
        CHECK(result.error.code == Code::Corrupt);
    }

    SECTION("alpha outside the unit range")
    {
        std::vector<std::byte> bad = good;
        StoreF32(bad, alphaOffset, 1.5f);
        Reseal(bad);
        const TerrainResult<TerrainEditDecodedTile> result = decode(bad);
        CHECK_FALSE(result);
        CHECK(result.error.code == Code::Corrupt);
    }

    SECTION("one flipped payload byte")
    {
        std::vector<std::byte> bad = good;
        bad[kPayloadOffset] = bad[kPayloadOffset] ^ std::byte{1};
        const TerrainResult<TerrainEditDecodedTile> result = decode(bad);
        CHECK_FALSE(result);
        CHECK(result.error.code == Code::Corrupt);
    }

    SECTION("unknown channel kind")
    {
        std::vector<std::byte> bad = good;
        StoreU32(bad, kHeaderSize + kKindOffset, 7);
        Reseal(bad);
        const TerrainResult<TerrainEditDecodedTile> result = decode(bad);
        CHECK_FALSE(result);
        CHECK(result.error.code == Code::Unsupported);
    }
}

TEST_CASE("A manifest round trips through JSON", "[terrain][edit]")
{
    const std::vector<TerrainEditLayerInfo> layers{
        {"base", -2, 0, true, TerrainEditBlend::Additive},
        {"detail", 3, 11, false, TerrainEditBlend::AlphaBlend},
    };

    const TerrainResult<std::vector<TerrainEditLayerInfo>> decoded =
        DecodeTerrainEditManifest(EncodeTerrainEditManifest(layers));
    REQUIRE(decoded);
    CHECK(*decoded.value == layers);
}

TEST_CASE("A manifest names every unknown, missing or duplicate field", "[terrain][edit]")
{
    using json = nlohmann::json;
    const std::vector<TerrainEditLayerInfo> layers{{"base", 0, 0, true, TerrainEditBlend::Additive}};
    const std::string text = EncodeTerrainEditManifest(layers);
    const auto decode = [](const std::string &manifest) { return DecodeTerrainEditManifest(manifest); };

    SECTION("unknown root field")
    {
        json manifest = json::parse(text);
        manifest["colour"] = "red";
        const TerrainResult<std::vector<TerrainEditLayerInfo>> result = decode(manifest.dump());
        CHECK_FALSE(result);
        CHECK(Mentions(result.error, "colour"));
    }

    SECTION("unknown layer field")
    {
        json manifest = json::parse(text);
        manifest["layers"][0]["opacity"] = 1;
        const TerrainResult<std::vector<TerrainEditLayerInfo>> result = decode(manifest.dump());
        CHECK_FALSE(result);
        CHECK(Mentions(result.error, "opacity"));
    }

    SECTION("missing field")
    {
        json manifest = json::parse(text);
        manifest["layers"][0].erase("priority");
        const TerrainResult<std::vector<TerrainEditLayerInfo>> result = decode(manifest.dump());
        CHECK_FALSE(result);
        CHECK(Mentions(result.error, "priority"));
    }

    SECTION("duplicate field")
    {
        std::string manifest = text;
        const std::string field = "\"name\": \"base\"";
        const size_t at = manifest.find(field);
        REQUIRE(at != std::string::npos);
        manifest.replace(at, field.size(), "\"name\": \"base\", \"name\": \"other\"");
        const TerrainResult<std::vector<TerrainEditLayerInfo>> result = decode(manifest);
        CHECK_FALSE(result);
        CHECK(Mentions(result.error, "name"));
    }

    SECTION("duplicate layer name")
    {
        const std::vector<TerrainEditLayerInfo> duplicate{{"base", 0, 0, true, TerrainEditBlend::Additive},
                                                         {"base", 1, 1, false, TerrainEditBlend::AlphaBlend}};
        const TerrainResult<std::vector<TerrainEditLayerInfo>> result =
            decode(EncodeTerrainEditManifest(duplicate));
        CHECK_FALSE(result);
        CHECK(Mentions(result.error, "name"));
    }
}

TEST_CASE("The stack hash ignores disabled layers and tile insertion order", "[terrain][edit]")
{
    TerrainEditLayer lower = Layer("lower", 0, 0, TerrainEditBlend::Additive);
    lower.tiles.emplace(TileCoord{0, 0}, FilledTile({0, 0}, 2, 1.0f));
    lower.tiles.emplace(TileCoord{1, 0}, FilledTile({1, 0}, 2, 2.0f));
    const std::array<TerrainEditLayer, 1> stack{lower};
    const uint64_t hash = TerrainEditStackHash(stack);

    TerrainEditLayer reversed = Layer("lower", 0, 0, TerrainEditBlend::Additive);
    reversed.tiles.emplace(TileCoord{1, 0}, FilledTile({1, 0}, 2, 2.0f));
    reversed.tiles.emplace(TileCoord{0, 0}, FilledTile({0, 0}, 2, 1.0f));
    const std::array<TerrainEditLayer, 1> reversedStack{reversed};
    CHECK(TerrainEditStackHash(reversedStack) == hash);

    TerrainEditLayer disabled = Layer("noise", 0, 0, TerrainEditBlend::Additive, false);
    disabled.tiles.emplace(TileCoord{0, 0}, FilledTile({0, 0}, 2, 99.0f));
    const std::array<TerrainEditLayer, 2> withDisabled{lower, disabled};
    CHECK(TerrainEditStackHash(withDisabled) == hash);
    const std::array<TerrainEditLayer, 1> disabledOnly{disabled};
    CHECK(TerrainEditStackHash(disabledOnly) == 0);
}

TEST_CASE("The stack hash changes with an edit, a blend mode or a compositing order", "[terrain][edit]")
{
    TerrainEditLayer lower = Layer("lower", 0, 0, TerrainEditBlend::Additive);
    lower.tiles.emplace(TileCoord{0, 0}, FilledTile({0, 0}, 2, 1.0f));
    const std::array<TerrainEditLayer, 1> stack{lower};
    const uint64_t hash = TerrainEditStackHash(stack);

    TerrainEditLayer edited = lower;
    edited.tiles[{0, 0}].values[4] = 5.0f;
    const std::array<TerrainEditLayer, 1> editedStack{edited};
    CHECK(TerrainEditStackHash(editedStack) != hash);

    TerrainEditLayer blended = lower;
    blended.info.heightBlend = TerrainEditBlend::AlphaBlend;
    const std::array<TerrainEditLayer, 1> blendedStack{blended};
    CHECK(TerrainEditStackHash(blendedStack) != hash);

    TerrainEditLayer first = Layer("first", 0, 0, TerrainEditBlend::Additive);
    first.tiles.emplace(TileCoord{0, 0}, FilledTile({0, 0}, 2, 1.0f));
    TerrainEditLayer second = Layer("second", 1, 0, TerrainEditBlend::Additive);
    second.tiles.emplace(TileCoord{0, 0}, FilledTile({0, 0}, 2, 2.0f));
    const std::array<TerrainEditLayer, 2> ordered{first, second};

    TerrainEditLayer moved = second;
    moved.info.priority = -1;
    const std::array<TerrainEditLayer, 2> swapped{first, moved};
    CHECK(TerrainEditStackHash(swapped) != TerrainEditStackHash(ordered));
}

TEST_CASE("An empty stack hash is zero", "[terrain][edit]")
{
    CHECK(TerrainEditStackHash(std::span<const TerrainEditLayer>()) == 0);
    TerrainEditLayer empty = Layer("empty", 0, 0, TerrainEditBlend::Additive);
    const std::array<TerrainEditLayer, 1> stack{empty};
    CHECK(TerrainEditStackHash(stack) == 0);
}
