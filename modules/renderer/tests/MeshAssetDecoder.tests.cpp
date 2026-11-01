/**
 * @file MeshAssetDecoder.tests.cpp
 * @author sumin.park
 * @brief Tests for renderer/MeshAssetDecoder: the mesh JSON parse, index check and bounds.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer/MeshAssetDecoder.h>

#include <assets/AssetBlob.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace mir;

namespace
{
    // Decodes JSON text the way a worker would, from a blob with no dependencies.
    struct Decoded
    {
        bool ok = false;
        DecodedMesh mesh;
        std::string error;
    };

    Decoded Decode(std::string_view json)
    {
        const std::vector<std::byte> raw =
            BuildAssetBlob(kRawAssetTypeTag, kRawAssetContentVersion, std::as_bytes(std::span(json)));
        const std::optional<AssetBlobView> blob = ParseAssetBlob(raw);
        REQUIRE(blob.has_value());

        const AssetDependencies none([](AssetId) -> const AssetBlobView * { return nullptr; });
        Decoded result;
        result.ok = kMeshAssetDecoder.decode(*blob, none, result.mesh, result.error);
        return result;
    }

    constexpr std::string_view kTriangle = R"({
        "vertices": [
            { "position": [-1, 0, 2], "color": [1, 1, 1], "normal": [0, 1, 0] },
            { "position": [3, 4, -5], "color": [1, 0, 0], "normal": [0, 1, 0] },
            { "position": [0, 1, 0], "color": [0, 1, 0], "normal": [0, 1, 0] }
        ],
        "indices": [0, 1, 2]
    })";
}

TEST_CASE("A mesh asset decodes to vertices, indices and bounds", "[renderer][mesh-decode]")
{
    const Decoded decoded = Decode(kTriangle);

    REQUIRE(decoded.ok);
    CHECK(decoded.mesh.vertices.size() == 3);
    CHECK(decoded.mesh.indices == std::vector<uint32_t>{ 0, 1, 2 });
    CHECK(decoded.mesh.vertices[1].pos.z == -5.0f);
    CHECK(decoded.mesh.bounds.mLocalMin == glm::vec3(-1.0f, 0.0f, -5.0f));
    CHECK(decoded.mesh.bounds.mLocalMax == glm::vec3(3.0f, 4.0f, 2.0f));
}

TEST_CASE("An empty mesh has zero bounds", "[renderer][mesh-decode]")
{
    const Decoded decoded = Decode("{}");

    REQUIRE(decoded.ok);
    CHECK(decoded.mesh.vertices.empty());
    CHECK(decoded.mesh.bounds.mLocalMin == glm::vec3(0.0f));
    CHECK(decoded.mesh.bounds.mLocalMax == glm::vec3(0.0f));
}

TEST_CASE("A mesh that is not JSON fails with a reason", "[renderer][mesh-decode]")
{
    const Decoded decoded = Decode("not json");

    CHECK_FALSE(decoded.ok);
    CHECK_FALSE(decoded.error.empty());
}

TEST_CASE("A vertex missing a field fails", "[renderer][mesh-decode]")
{
    const Decoded decoded = Decode(R"({ "vertices": [ { "position": [0, 0, 0], "color": [1, 1, 1] } ] })");

    CHECK_FALSE(decoded.ok);
    CHECK_FALSE(decoded.error.empty());
}

TEST_CASE("An index past the vertices fails", "[renderer][mesh-decode]")
{
    const Decoded decoded = Decode(R"({
        "vertices": [ { "position": [0, 0, 0], "color": [1, 1, 1], "normal": [0, 1, 0] } ],
        "indices": [0, 0, 1]
    })");

    CHECK_FALSE(decoded.ok);
    CHECK(decoded.error.find("index 1") != std::string::npos);
}

TEST_CASE("Decoding again into a used result starts clean", "[renderer][mesh-decode]")
{
    const std::string_view json = kTriangle;
    const std::vector<std::byte> raw =
        BuildAssetBlob(kRawAssetTypeTag, kRawAssetContentVersion, std::as_bytes(std::span(json)));
    const std::optional<AssetBlobView> blob = ParseAssetBlob(raw);
    REQUIRE(blob.has_value());
    const AssetDependencies none([](AssetId) -> const AssetBlobView * { return nullptr; });

    DecodedMesh mesh;
    std::string error;
    REQUIRE(kMeshAssetDecoder.decode(*blob, none, mesh, error));
    REQUIRE(kMeshAssetDecoder.decode(*blob, none, mesh, error));

    CHECK(mesh.vertices.size() == 3);
    CHECK(mesh.indices.size() == 3);
}
