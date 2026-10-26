/**
 * @file SurfaceMaterial.tests.cpp
 * @author Jackey Cen
 * @brief Tests for assets/SurfaceMaterialAsset.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <assets/ModelAsset.h>

#include <catch2/catch_test_macros.hpp>

using namespace mir;

namespace
{
    SurfaceMaterialAsset MakeMaterial()
    {
        SurfaceMaterialAsset material;
        material.shaderFile = "materials/rock.slang";
        material.shaderType = "RockMaterial";
        material.frontends = { "mesh" };
        material.parametersJson = R"({"roughness":0.8})";
        material.textures.push_back(SurfaceMaterialTexture{
            .role = "baseColor", .path = "textures/rock.jpg", .colorSpace = SurfaceTextureColorSpace::Srgb });
        material.resolution = 1024;
        return material;
    }
}

TEST_CASE("SurfaceMaterialAsset round-trips its versioned payload", "[assets][material]")
{
    const SurfaceMaterialAsset material = MakeMaterial();
    const std::vector<std::byte> blob = BuildTypedAsset(material);
    const std::optional<AssetBlobView> view = ParseAssetBlob(blob);
    REQUIRE(view.has_value());
    CHECK(view->header.typeTag == kSurfaceMaterialAssetTypeTag);
    CHECK(view->header.contentVersion == kSurfaceMaterialAssetContentVersion);

    SurfaceMaterialAsset loaded;
    std::string error;
    REQUIRE(ReadTypedAsset(*view, loaded, error));
    CHECK(error.empty());
    CHECK(loaded.version == 1);
    CHECK(loaded.shaderFile == material.shaderFile);
    CHECK(loaded.shaderType == material.shaderType);
    CHECK(loaded.frontends == material.frontends);
    CHECK(loaded.parametersJson == material.parametersJson);
    CHECK(loaded.textures.size() == 1);
    CHECK(loaded.textures[0].role == "baseColor");
    CHECK(loaded.textures[0].path == "textures/rock.jpg");
    CHECK(loaded.textures[0].colorSpace == SurfaceTextureColorSpace::Srgb);
    CHECK(loaded.resolution == 1024);
}

TEST_CASE("SurfaceMaterialAsset rejects an unknown frontend", "[assets][material]")
{
    SurfaceMaterialAsset material = MakeMaterial();
    material.frontends = { "skinned" };
    std::string error;
    CHECK_FALSE(ValidateAsset(material, error));
    CHECK_FALSE(error.empty());
}

TEST_CASE("SurfaceMaterialAsset rejects a partial packed texture set", "[assets][material]")
{
    SurfaceMaterialAsset material = MakeMaterial();
    material.albedoTexture = AssetId{ 7 };
    std::string error;
    CHECK_FALSE(ValidateAsset(material, error));
}
