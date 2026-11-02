/**
 * @file AssetReferences.tests.cpp
 * @author sumin.park
 * @brief Tests for CollectAssetReferences across the cooked asset types.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "../AssetReferences.h"

#include <assets/AssetBlob.h>
#include <assets/ModelAsset.h>
#include <particles/ParticleEffect.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <vector>

using namespace mir;

namespace
{
    AssetId Id(const char *name) { return MakeAssetId(name); }

    std::vector<AssetId> References(const std::vector<std::byte> &bytes)
    {
        const auto blob = ParseAssetBlob(bytes);
        REQUIRE(blob);
        return cook::CollectAssetReferences(*blob);
    }

    ModelNode Node(const char *key, AssetId mesh)
    {
        ModelNode node;
        node.key = key;
        node.name = key;
        node.mesh = mesh;
        return node;
    }

    MeshPrimitive Triangle(AssetId material, AssetId texture)
    {
        MeshPrimitive primitive;
        primitive.material = material;
        primitive.baseColorTexture = texture;
        primitive.vertices.resize(3);
        primitive.indices = {0, 1, 2};
        return primitive;
    }
}

TEST_CASE("A model lists its meshes, skeletons and clips once each", "[cooker][references]")
{
    ModelAsset model;
    model.nodes = {Node("a", Id("mesh/one")), Node("b", Id("mesh/one")), Node("c", Id("mesh/two")), Node("d", kNullAssetId)};
    model.nodes[2].skin = 0;
    model.scenes = {{"scene", {0}}};
    model.skins = {{Id("skeleton/one"), {"a"}, {glm::mat4(1)}}};
    model.animations = {Id("clip/one"), Id("clip/two"), Id("clip/one")};

    const std::vector<AssetId> expected{Id("mesh/one"), Id("mesh/two"), Id("skeleton/one"), Id("clip/one"), Id("clip/two")};
    CHECK(References(BuildTypedAsset(model)) == expected);
}

TEST_CASE("A mesh lists its materials and textures once each", "[cooker][references]")
{
    MeshAsset mesh;
    mesh.primitives = {Triangle(Id("material/one"), Id("texture/one")), Triangle(Id("material/two"), Id("texture/one")),
                       Triangle(kNullAssetId, kNullAssetId)};

    const std::vector<AssetId> expected{Id("material/one"), Id("texture/one"), Id("material/two")};
    CHECK(References(BuildTypedAsset(mesh)) == expected);
}

TEST_CASE("A material lists its texture and a textureless one lists nothing", "[cooker][references]")
{
    MaterialAsset textured;
    textured.baseColorTexture = Id("texture/one");
    const std::vector<AssetId> expected{Id("texture/one")};
    CHECK(References(BuildTypedAsset(textured)) == expected);

    CHECK(References(BuildTypedAsset(MaterialAsset{})).empty());
}

TEST_CASE("A surface material lists its packed textures", "[cooker][references]")
{
    SurfaceMaterialAsset material;
    material.shaderFile = "shaders/default.slang";
    material.shaderType = "surface";
    material.frontends = {"mesh"};
    material.albedoTexture = Id("packed/albedo");
    material.normalTexture = Id("packed/normal");
    material.surfaceTexture = Id("packed/surface");

    const std::vector<AssetId> expected{Id("packed/albedo"), Id("packed/normal"), Id("packed/surface")};
    CHECK(References(BuildTypedAsset(material)) == expected);
}

TEST_CASE("A particle emitter lists its texture, script and layout", "[cooker][references]")
{
    auto source = ReadParticleEmitterSource(
        R"({"version":3,"target":"gpu","lifetime":2,"spawnRate":60,"capacity":256,"texture":"textures/white.png"})", "assets");
    REQUIRE(source.mEmitter);

    SECTION("with a generated script")
    {
        source.mEmitter->script = Id("script/one");
        source.mEmitter->scriptLayout = Id("layout/one");
        const std::vector<AssetId> expected{MakeAssetId("assets/textures/white.png"), Id("script/one"), Id("layout/one")};
        CHECK(References(BuildParticleEmitterAsset(*source.mEmitter)) == expected);
    }

    SECTION("without one")
    {
        const std::vector<AssetId> expected{MakeAssetId("assets/textures/white.png")};
        CHECK(References(BuildParticleEmitterAsset(*source.mEmitter)) == expected);
    }
}

TEST_CASE("A particle effect lists its emitters", "[cooker][references]")
{
    const auto source = ReadParticleEffectSource(
        R"({"version":3,"placements":[{"id":1,"emitter":"a.emitter.json","offset":[0,0,0]},
            {"id":2,"emitter":"b.emitter.json","offset":[0,0,0]}]})", "assets");
    REQUIRE(source.mEffect);

    const std::vector<AssetId> expected{MakeAssetId("assets/a.emitter.json"), MakeAssetId("assets/b.emitter.json")};
    CHECK(References(BuildParticleEffectAsset(*source.mEffect)) == expected);
}

TEST_CASE("A type with no references lists nothing", "[cooker][references]")
{
    CHECK(References(BuildAssetBlob(kRawAssetTypeTag, kRawAssetContentVersion, {})).empty());
    CHECK(References(BuildAssetBlob(0x12345678u, 1, {})).empty());
}

TEST_CASE("A payload that does not decode is an error", "[cooker][references]")
{
    const std::vector<std::byte> garbage(7, std::byte{0xFF});
    const auto bytes = BuildAssetBlob(kModelAssetTypeTag, TypedAssetVersion(kModelAssetTypeTag), garbage);
    const auto blob = ParseAssetBlob(bytes);
    REQUIRE(blob);
    CHECK_THROWS(cook::CollectAssetReferences(*blob));
}
