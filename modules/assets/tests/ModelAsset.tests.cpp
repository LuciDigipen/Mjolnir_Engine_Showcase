/**
 * @file ModelAsset.tests.cpp
 * @author Jackey Cen
 * @brief Tests for assets/ModelAsset.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <assets/AssetBlob.h>
#include <assets/ModelAsset.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <string>
#include <vector>

using namespace mir;

TEST_CASE("PBR material factors and all texture bindings round trip", "[assets][pbr]")
{
    MaterialAsset material;
    material.metallic = .3f;
    material.roughness = .7f;
    material.normalScale = -.5f;
    material.ior = 1.8f;
    material.clearcoat = .4f;
    material.clearcoatNormalScale = .6f;
    material.emissive = glm::vec3(2, 3, 4);
    material.specularColor = glm::vec3(.2f, .4f, .6f);
    for (size_t i = 0; i < material.textures.size(); ++i)
    {
        material.textures[i].texture = AssetId{100 + i};
        material.textures[i].wrapT = 33071;
    }
    const auto bytes = BuildTypedAsset(material);
    const auto view = ParseAssetBlob(bytes);
    REQUIRE(view);
    MaterialAsset loaded;
    std::string error;
    REQUIRE(ReadTypedAsset(*view, loaded, error));
    CHECK(loaded.metallic == material.metallic);
    CHECK(loaded.normalScale == material.normalScale);
    CHECK(loaded.specularColor == material.specularColor);
    CHECK(loaded.emissive == material.emissive);
    CHECK(loaded.clearcoatNormalScale == material.clearcoatNormalScale);
    for (size_t i = 0; i < material.textures.size(); ++i)
    {
        CHECK(loaded.textures[i].texture == material.textures[i].texture);
        CHECK(loaded.textures[i].wrapT == 33071);
    }
    loaded.ior = 0;
    loaded.specularColor = glm::vec3(2);
    CHECK(ValidateAsset(loaded, error));
    loaded.roughness = 2;
    CHECK_FALSE(ValidateAsset(loaded, error));
}

namespace
{
    TextureAsset MakeTexture()
    {
        TextureAsset texture;
        texture.width = texture.height = 2;
        texture.format = TextureFormat::Rgba8Srgb;
        texture.colorSpace = TextureColorSpace::Srgb;
        texture.rgba = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
        texture.mips = {{1, 1, 0, 4}};
        texture.mipData = {17, 18, 19, 20};
        return texture;
    }
    SurfaceMaterialAsset MakeMaterial()
    {
        SurfaceMaterialAsset material;
        material.shaderFile = "shaders/default.slang";
        material.shaderType = "surface";
        material.frontends = {"mesh", "terrain"};
        material.parametersJson = R"({"tint":[1,1,1,1]})";
        material.textures = {{"baseColor", "textures/a.png", TextureColorSpace::Srgb}};
        material.albedoTexture = MakeAssetId("materials/a.material.json#albedoTexture");
        material.normalTexture = MakeAssetId("materials/a.material.json#normalTexture");
        material.surfaceTexture = MakeAssetId("materials/a.material.json#surfaceTexture");
        material.unknownJson = R"({"custom":{"kept":true}})";
        return material;
    }
} // namespace

TEST_CASE("Texture payload round-trips its format and complete mip chain", "[assets][texture]")
{
    const TextureAsset original = MakeTexture();
    const std::vector<std::byte> bytes = BuildTypedAsset(original);
    const auto view = ParseAssetBlob(bytes);
    REQUIRE(view);
    CHECK(view->header.contentVersion == kTextureAssetContentVersion);

    TextureAsset loaded;
    std::string error;
    REQUIRE(ReadTypedAsset(*view, loaded, error));
    CHECK(loaded.width == 2);
    CHECK(loaded.format == TextureFormat::Rgba8Srgb);
    CHECK(loaded.colorSpace == TextureColorSpace::Srgb);
    CHECK(loaded.rgba == original.rgba);
    REQUIRE(loaded.mips.size() == 1);
    CHECK(loaded.mips[0].offset == 0);
    CHECK(loaded.mips[0].length == 4);
    CHECK(TextureMipCount(loaded) == 2);
    const auto pixels = TextureMipPixels(loaded, 1);
    CHECK(std::vector<uint8_t>(pixels.begin(), pixels.end()) == loaded.mipData);
}

TEST_CASE("Texture payload rejects missing, partial and old-version data", "[assets][texture]")
{
    TextureAsset texture = MakeTexture();
    texture.rgba.pop_back();
    CHECK_THROWS(BuildTypedAsset(texture));

    texture = MakeTexture();
    texture.mips.clear();
    CHECK_THROWS(BuildTypedAsset(texture));

    texture = MakeTexture();
    texture.mips[0].width = 2;
    CHECK_THROWS(BuildTypedAsset(texture));

    texture = MakeTexture();
    const std::vector<std::byte> current = BuildTypedAsset(texture);
    const auto view = ParseAssetBlob(current);
    REQUIRE(view);
    std::vector<std::byte> old = std::vector<std::byte>(current.begin(), current.begin() + view->content.size());
    const std::vector<std::byte> blob = BuildAssetBlob(kTextureAssetTypeTag, 1, old);
    TextureAsset loaded;
    std::string error;
    CHECK_FALSE(ReadTypedAsset(*ParseAssetBlob(blob), loaded, error));
    CHECK(error == "unsupported typed asset/version");

    std::vector<std::byte> truncated(current.begin(), current.end() - 2);
    CHECK_FALSE(ParseAssetBlob(truncated));
}

TEST_CASE("Surface material payload round-trips parameters, paths and packed ids", "[assets][material]")
{
    const SurfaceMaterialAsset original = MakeMaterial();
    const std::vector<std::byte> bytes = BuildTypedAsset(original);
    const auto view = ParseAssetBlob(bytes);
    REQUIRE(view);
    CHECK(view->header.typeTag == kSurfaceMaterialAssetTypeTag);
    CHECK(view->header.contentVersion == kSurfaceMaterialAssetContentVersion);

    SurfaceMaterialAsset loaded;
    std::string error;
    REQUIRE(ReadTypedAsset(*view, loaded, error));
    CHECK(loaded.shaderFile == original.shaderFile);
    CHECK(loaded.shaderType == original.shaderType);
    CHECK(loaded.frontends == original.frontends);
    CHECK(loaded.parametersJson == original.parametersJson);
    CHECK(loaded.unknownJson == original.unknownJson);
    CHECK(loaded.albedoTexture == original.albedoTexture);
    CHECK(loaded.normalTexture == original.normalTexture);
    CHECK(loaded.surfaceTexture == original.surfaceTexture);
    REQUIRE(loaded.textures.size() == 1);
    CHECK(loaded.textures[0].role == "baseColor");
    CHECK(loaded.textures[0].colorSpace == TextureColorSpace::Srgb);
}

TEST_CASE("Surface material payload rejects unsupported schema values", "[assets][material]")
{
    SurfaceMaterialAsset material = MakeMaterial();
    material.version = 2;
    CHECK_THROWS(BuildTypedAsset(material));

    material = MakeMaterial();
    material.frontends = {"unlit"};
    CHECK_THROWS(BuildTypedAsset(material));

    material = MakeMaterial();
    material.textures[0].role = "metalness";
    CHECK_THROWS(BuildTypedAsset(material));

    material = MakeMaterial();
    material.surfaceTexture = kNullAssetId;
    CHECK_THROWS(BuildTypedAsset(material));

    material = MakeMaterial();
    material.resolution = 0;
    CHECK_THROWS(BuildTypedAsset(material));
}
