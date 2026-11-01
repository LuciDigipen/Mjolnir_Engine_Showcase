/**
 * @file MaterialCooker.tests.cpp
 * @author Jackey Cen
 * @brief Tests for tools/AssetCooker/MaterialCooker.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "CookTestSupport.h"
#include "../MaterialCooker.h"
#include <assets/AssetCache.h>
#include <assets/AssetManifest.h>
#include <assets/ModelAsset.h>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <core/fs/FileIO.h>
#include <core/platform/Process.h>
#include <fstream>
#include <nlohmann/json.hpp>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

using namespace mir;
namespace fs = std::filesystem;
using Json = nlohmann::json;

namespace
{
    struct MaterialFixture
    {
        fs::path root;
        MaterialFixture()
        {
            root = fs::current_path() /
                   ("material-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            fs::create_directories(root / "materials");
            fs::create_directories(root / "textures");
            fs::create_directories(root / "shaders");
            REQUIRE(WriteText(root / "shaders/test.slang", "// test shader"));
            WriteSolid("textures/base.png", 188, 188, 188, 255);
            WriteSolid("textures/ao.png", 64, 64, 64, 255);
            WriteSolid("textures/roughness.png", 200, 200, 200, 255);
            WriteSolid("textures/displacement.png", 100, 100, 100, 255);
            WriteSolid("textures/specular.png", 188, 188, 188, 255);
            WriteNormal();
            WriteMaterial();
        }
        void WriteSolid(const char *path, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
        {
            std::vector<uint8_t> pixels(4 * 4 * 4);
            for (size_t i = 0; i < pixels.size(); i += 4)
            {
                pixels[i] = r;
                pixels[i + 1] = g;
                pixels[i + 2] = b;
                pixels[i + 3] = a;
            }
            REQUIRE(stbi_write_png((root / path).string().c_str(), 4, 4, 4, pixels.data(), 16) != 0);
        }
        void WriteNormal()
        {
            const uint8_t plus[] = {255, 127, 127, 255};
            const uint8_t minus[] = {0, 128, 128, 255};
            std::vector<uint8_t> ordered;
            for (int i = 0; i < 2; ++i)
                ordered.insert(ordered.end(), plus, plus + 4);
            for (int i = 0; i < 2; ++i)
                ordered.insert(ordered.end(), minus, minus + 4);
            REQUIRE(stbi_write_png((root / "textures/normal.png").string().c_str(), 2, 2, 4, ordered.data(), 8) != 0);
        }
        void WriteMaterial(int resolution = 2)
        {
            Json material = {
                {"version", 1},
                {"shader", {{"file", "shaders/test.slang"}, {"type", "surface"}}},
                {"frontends", {"mesh", "terrain"}},
                {"parameters", {{"tint", {1, 1, 1, 1}}}},
                {"resolution", resolution},
                {"textures",
                 {{"baseColor", {{"path", "textures/base.png"}, {"colorSpace", "srgb"}}},
                  {"normal", {{"path", "textures/normal.png"}, {"colorSpace", "linear"}}},
                  {"roughness", {{"path", "textures/roughness.png"}, {"colorSpace", "linear"}}},
                  {"displacement", {{"path", "textures/displacement.png"}, {"colorSpace", "linear"}}},
                  {"specular", {{"path", "textures/specular.png"}, {"colorSpace", "srgb"}}},
                  {"ao", {{"path", "textures/ao.png"}, {"colorSpace", "srgb"}}}}},
                {"editorOnly", {{"note", "kept"}}}};
            std::ofstream(root / "materials/sand.material.json") << material.dump(2);
        }
        std::vector<std::string> Args(const fs::path &out, const fs::path &result) const
        {
            return {"--source-dir", root.string(), "--id-root", "assets", "--out", out.string(), "--result",
                    result.string()};
        }
    };

    const TextureAsset &LoadTexture(AssetCache &cache, AssetId id, TextureAsset &storage)
    {
        std::string error;
        REQUIRE(LoadTypedAsset(cache, id, storage, error));
        return storage;
    }
    struct MaterialSnapshot
    {
        SurfaceMaterialAsset material;
        TextureAsset albedo, normal, surface;
    };
    MaterialSnapshot LoadSnapshot(const fs::path &out, const std::string &materialPath)
    {
        const auto manifest = AssetManifest::LoadFile(out / "manifest.blob");
        REQUIRE(manifest);
        AssetCache cache(&*manifest, out);
        MaterialSnapshot snapshot;
        std::string error;
        REQUIRE(LoadTypedAsset(cache, test::PathId(*manifest, materialPath), snapshot.material, error));
        LoadTexture(cache, snapshot.material.albedoTexture, snapshot.albedo);
        LoadTexture(cache, snapshot.material.normalTexture, snapshot.normal);
        LoadTexture(cache, snapshot.material.surfaceTexture, snapshot.surface);
        return snapshot;
    }
} // namespace

TEST_CASE("Surface material packs base color, AO, normals and F0 into three mip chains", "[material][cook]")
{
    MaterialFixture fixture;
    const auto out = fs::path(fixture.root.string() + "-cooked");
    const auto result = out / "result.json";
    REQUIRE(LaunchProcess(GLTF_COOKER_EXE, fixture.Args(out, result), true) == 0);

    const auto manifest = AssetManifest::LoadFile(out / "manifest.blob");
    REQUIRE(manifest);
    const MaterialSnapshot snapshot = LoadSnapshot(out, "assets/materials/sand.material.json");
    const SurfaceMaterialAsset &material = snapshot.material;
    CHECK(material.shaderFile == "shaders/test.slang");
    CHECK(material.frontends == std::vector<std::string>{"mesh", "terrain"});
    CHECK(material.parametersJson == R"({"tint":[1,1,1,1]})");
    CHECK(material.unknownJson == R"({"editorOnly":{"note":"kept"}})");
    CHECK(material.resolution == 2);
    REQUIRE_FALSE(material.albedoTexture.IsNull());
    REQUIRE_FALSE(material.normalTexture.IsNull());
    REQUIRE_FALSE(material.surfaceTexture.IsNull());

    const TextureAsset &albedo = snapshot.albedo;
    const TextureAsset &normal = snapshot.normal;
    const TextureAsset &surface = snapshot.surface;
    for (const TextureAsset *texture : {&albedo, &normal, &surface})
    {
        CHECK(texture->width == 2);
        CHECK(texture->height == 2);
        CHECK(TextureMipCount(*texture) == 2);
        CHECK(texture->mips.back().width == 1);
        CHECK(texture->mips.back().height == 1);
    }
    CHECK(albedo.format == TextureFormat::Rgba8Srgb);
    CHECK(albedo.colorSpace == TextureColorSpace::Srgb);
    CHECK(albedo.rgba[0] == 188);
    CHECK(albedo.rgba[3] == 64);
    CHECK(albedo.mipData[3] == 64);
    CHECK(normal.format == TextureFormat::Rgba8Unorm);
    CHECK(normal.colorSpace == TextureColorSpace::Linear);
    // Opposing normals average to zero, which becomes the flat normal.
    CHECK(normal.mipData[0] == 128);
    CHECK(normal.mipData[1] == 128);
    CHECK(normal.mipData[2] == 255);
    CHECK(surface.rgba[0] == 200);
    CHECK(surface.rgba[1] == 100);
    CHECK(surface.rgba[2] == Catch::Approx(128).margin(1));

    // The six source images are inputs to this pack, not standalone 4K textures.
    for (const char *source : {"base", "ao", "roughness", "displacement", "specular", "normal"})
        CHECK(manifest->FindByPath(std::string("assets/textures/") + source + ".png") == nullptr);
    CHECK(manifest->FindByPath("assets/materials/sand.material.json") != nullptr);

    const Json cooked = Json::parse(std::ifstream(result));
    REQUIRE(cooked.at("models").size() == 1);
    const Json &entry = cooked.at("models").at(0);
    CHECK(entry.at("status") == "cooked");
    CHECK(entry.at("sizes").size() == 3);
    for (const auto &size : entry.at("sizes"))
    {
        CHECK(size.at("width") == 2);
        CHECK(size.at("height") == 2);
    }
}

TEST_CASE("Material batches skip unchanged inputs and recook edited sources", "[material][cook][incremental]")
{
    MaterialFixture fixture;
    const auto out = fs::path(fixture.root.string() + "-incremental");
    const auto result = out / "result.json";
    const std::string materialPath = "assets/materials/sand.material.json";
    REQUIRE(LaunchProcess(GLTF_COOKER_EXE, fixture.Args(out, result), true) == 0);

    auto run = [&](std::vector<std::string> extra)
        {
            auto args = fixture.Args(out, result);
            args.insert(args.end(), extra.begin(), extra.end());
            return LaunchProcess(GLTF_COOKER_EXE, args, true);
        };
    {
        const auto manifest = AssetManifest::LoadFile(out / "manifest.blob");
        REQUIRE(manifest);
        CHECK(manifest->Count() == 4);
    }
    REQUIRE(run({"--material", materialPath}) == 0);
    CHECK(Json::parse(std::ifstream(result)).at("status") == "unchanged");

    fixture.WriteSolid("textures/base.png", 64, 64, 64, 255);
    REQUIRE(run({"--changed", (fixture.root / "textures/base.png").string()}) == 0);
    CHECK(Json::parse(std::ifstream(result)).at("status") == "succeeded");
    MaterialSnapshot edited = LoadSnapshot(out, materialPath);
    CHECK(edited.albedo.rgba[0] == 64);

    fixture.WriteMaterial(1);
    REQUIRE(run({"--changed", (fixture.root / "materials/sand.material.json").string()}) == 0);
    MaterialSnapshot resized = LoadSnapshot(out, materialPath);
    CHECK(resized.material.resolution == 1);
    CHECK(resized.surface.width == 1);
    CHECK(TextureMipCount(resized.surface) == 1);

    std::ofstream(fixture.root / "textures/base.png") << "version https://git-lfs.github.com/spec/v1\n";
    CHECK(run({"--material", materialPath}) != 0);
    const Json failed = Json::parse(std::ifstream(result));
    CHECK(failed.at("status") == "failed");
    CHECK(failed.at("diagnostic").at("message").get<std::string>().find("Git LFS pointer") != std::string::npos);
}

TEST_CASE("Changed batches rebuild incompatible textures when the import cache is reset", "[material][cook][incremental]")
{
    MaterialFixture fixture;
    fixture.WriteSolid("textures/extra.png", 90, 90, 90, 255);
    const auto out = fs::path(fixture.root.string() + "-cache-upgrade");
    const auto result = out / "result.json";
    REQUIRE(LaunchProcess(GLTF_COOKER_EXE, fixture.Args(out, result), true) == 0);

    const auto manifest = AssetManifest::LoadFile(out / "manifest.blob");
    REQUIRE(manifest);
    const AssetId extraId = test::PathId(*manifest, "assets/textures/extra.png");
    REQUIRE(manifest->Find(extraId));
    std::vector<std::byte> legacyPixels(16);
    legacyPixels[0] = std::byte{1};
    legacyPixels[4] = std::byte{1};
    legacyPixels[8] = std::byte{4};
    legacyPixels[15] = std::byte{255};
    REQUIRE(WriteBytes(out / "legacy-texture.blob", BuildAssetBlob(kTextureAssetTypeTag, 1, legacyPixels)));
    std::vector<AssetManifestSourceEntry> entries;
    for (const auto &entry : manifest->Entries())
    {
        if (entry.id == extraId)
            entries.push_back({{entry.id, entry.typeTag, 1}, "legacy-texture.blob", manifest->LogicalPathOf(entry),
                manifest->Dependencies(entry)});
        else
            entries.push_back({entry, manifest->PathOf(entry), manifest->LogicalPathOf(entry),
                manifest->Dependencies(entry)});
    }
    REQUIRE(WriteBytes(out / "manifest.blob", BuildAssetManifestBlob(entries)));
    const auto cachePath = out / "import-cache.json";
    Json cache = Json::parse(std::ifstream(cachePath));
    cache["version"] = 3;
    cache.erase("materials");
    REQUIRE(WriteText(cachePath, cache.dump()));
    auto args = fixture.Args(out, result);
    args.insert(args.end(), {"--changed", (fixture.root / "textures/base.png").string()});

    SECTION("An older cache version requires a complete recook") {}
    SECTION("A missing cache requires a complete recook")
    {
        REQUIRE(fs::remove(cachePath));
    }
    SECTION("A failed upgrade keeps the complete recook pending")
    {
        REQUIRE(WriteText(fixture.root / "textures/base.png", "version https://git-lfs.github.com/spec/v1\n"));
        REQUIRE(LaunchProcess(GLTF_COOKER_EXE, args, true) != 0);
        const auto failedManifest = AssetManifest::LoadFile(out / "manifest.blob");
        REQUIRE(failedManifest);
        REQUIRE(failedManifest->Find(extraId));
        CHECK(failedManifest->Find(extraId)->contentVersion == 1);
        CHECK(Json::parse(std::ifstream(cachePath)).at("requiresFullCook") == true);
    }
    fixture.WriteSolid("textures/base.png", 64, 64, 64, 255);
    REQUIRE(LaunchProcess(GLTF_COOKER_EXE, args, true) == 0);
    CHECK(Json::parse(std::ifstream(result)).at("status") == "succeeded");
    CHECK(LoadSnapshot(out, "assets/materials/sand.material.json").albedo.rgba[0] == 64);
    const auto upgraded = AssetManifest::LoadFile(out / "manifest.blob");
    REQUIRE(upgraded);
    REQUIRE(upgraded->Find(extraId));
    CHECK(upgraded->Find(extraId)->contentVersion == kTextureAssetContentVersion);
    AssetCache upgradedCache(&*upgraded, out);
    TextureAsset extra;
    LoadTexture(upgradedCache, extraId, extra);
    CHECK(extra.width == 4);
    CHECK(extra.rgba[0] == 90);
    const auto rebuiltCache = Json::parse(std::ifstream(cachePath));
    CHECK_FALSE(rebuiltCache.contains("requiresFullCook"));
    CHECK(rebuiltCache.at("materials").contains("assets/materials/sand.material.json"));
}

TEST_CASE("A material cook drops previously cooked source image entries", "[material][cook][incremental]")
{
    MaterialFixture fixture;
    const auto out = fs::path(fixture.root.string() + "-cleanup");
    const auto result = out / "result.json";
    const std::string materialPath = "assets/materials/sand.material.json";
    fs::remove(fixture.root / "materials/sand.material.json");
    REQUIRE(LaunchProcess(GLTF_COOKER_EXE, fixture.Args(out, result), true) == 0);
    {
        const auto manifest = AssetManifest::LoadFile(out / "manifest.blob");
        REQUIRE(manifest);
        CHECK(manifest->Count() == 6);
        CHECK(manifest->FindByPath("assets/textures/base.png") != nullptr);
    }

    fixture.WriteMaterial();
    auto args = fixture.Args(out, result);
    args.insert(args.end(), {"--material", materialPath});
    REQUIRE(LaunchProcess(GLTF_COOKER_EXE, args, true) == 0);
    const auto manifest = AssetManifest::LoadFile(out / "manifest.blob");
    REQUIRE(manifest);
    CHECK(manifest->Count() == 4);
    for (const char *source : {"base", "ao", "roughness", "displacement", "specular", "normal"})
        CHECK(manifest->FindByPath(std::string("assets/textures/") + source + ".png") == nullptr);
    CHECK(manifest->FindByPath(materialPath) != nullptr);
}

TEST_CASE("A glTF package move refuses a surface material before moving files", "[material][cook][move]")
{
    MaterialFixture fixture;
    const auto out = fs::path(fixture.root.string() + "-move");
    const auto result = out / "result.json";
    const auto source = fixture.root / "materials/sand.material.json";
    const auto before = ReadBytes(source);
    REQUIRE(before);
    auto args = fixture.Args(out, result);
    args.insert(args.end(), {"--move-model", "assets/materials/sand.material.json", "--move-to", "moved"});
    REQUIRE(LaunchProcess(GLTF_COOKER_EXE, args, true) != 0);
    CHECK(ReadBytes(source) == before);
    CHECK_FALSE(fs::exists(fixture.root / "moved"));
    const Json failed = Json::parse(std::ifstream(result));
    CHECK(failed.at("diagnostic").at("message").get<std::string>().find("cannot be moved") != std::string::npos);
}

TEST_CASE("The authorized TerrainCheck set cooks at the default 2K resolution", "[material][real]")
{
    const auto sourceRoot = fs::path(MJOLNIR_SOURCE_DIR);
    const auto gameRoot = sourceRoot / "games/TerrainCheck/assets";
    const auto engineRoot = sourceRoot / "assets";
    const auto stamp = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto root = fs::temp_directory_path() / ("terraincheck-material-fixture-" + stamp);
    const auto out = fs::temp_directory_path() / ("terraincheck-material-cooked-" + stamp);
    fs::create_directories(root / "materials");
    const Json material = {
        {"version", 1},
        {"shader", {{"file", "shaders/default.slang"}, {"type", "surface"}}},
        {"frontends", {"terrain"}},
        {"textures",
         {{"baseColor", {{"path", "textures/icelandic_sand_and_rocks/Icelandic_Sand_And_Rocks_tcigfcgn_4K_BaseColor.jpg"}, {"colorSpace", "srgb"}}},
          {"normal", {{"path", "textures/icelandic_sand_and_rocks/Icelandic_Sand_And_Rocks_tcigfcgn_4K_Normal.jpg"}, {"colorSpace", "linear"}}},
          {"displacement", {{"path", "textures/icelandic_sand_and_rocks/Icelandic_Sand_And_Rocks_tcigfcgn_4K_Displacement.jpg"}, {"colorSpace", "linear"}}},
          {"roughness", {{"path", "textures/icelandic_sand_and_rocks/Icelandic_Sand_And_Rocks_tcigfcgn_4K_Roughness.jpg"}, {"colorSpace", "linear"}}},
          {"specular", {{"path", "textures/icelandic_sand_and_rocks/Icelandic_Sand_And_Rocks_tcigfcgn_4K_Specular.jpg"}, {"colorSpace", "srgb"}}},
          {"ao", {{"path", "textures/icelandic_sand_and_rocks/Icelandic_Sand_And_Rocks_tcigfcgn_4K_AO.jpg"}, {"colorSpace", "srgb"}}}}}};
    // A fresh cache cooks every declared root. Keep this fixture independent
    // of other materials that the game or engine adds later.
    fs::create_directories(root / "shaders");
    fs::copy_file(engineRoot / "shaders/default.slang", root / "shaders/default.slang");
    for (const auto &texture : material.at("textures"))
    {
        const fs::path relative = texture.at("path").get<std::string>();
        fs::create_directories((root / relative).parent_path());
        fs::copy_file(gameRoot / relative, root / relative);
    }
    std::ofstream(root / "materials/icelandic_sand.material.json") << material.dump(2);

    const auto result = out / "result.json";
    const std::vector<std::string> args{"--source-dir", root.string(), "--id-root", "fixture",
                                        "--out", out.string(), "--result", result.string(), "--material",
                                        "fixture/materials/icelandic_sand.material.json"};
    REQUIRE(LaunchProcess(GLTF_COOKER_EXE, args, true) == 0);
    const Json cooked = Json::parse(std::ifstream(result));
    REQUIRE(cooked.at("models").size() == 1);
    const Json &entry = cooked.at("models").at(0);
    CHECK(entry.at("status") == "cooked");
    REQUIRE(entry.at("sizes").size() == 3);
    for (const auto &size : entry.at("sizes"))
    {
        CHECK(size.at("width") == 2048);
        CHECK(size.at("height") == 2048);
    }
    const auto manifest = AssetManifest::LoadFile(out / "manifest.blob");
    REQUIRE(manifest);
    CHECK(manifest->FindByPath("fixture/materials/icelandic_sand.material.json") != nullptr);
    CHECK(manifest->Count() == 4);
    std::error_code error;
    fs::remove_all(root, error);
    fs::remove_all(out, error);
}

TEST_CASE("A changed .meta guid recooks the material under the new id", "[material][cook][incremental]")
{
    MaterialFixture fixture;
    const auto out = fs::path(fixture.root.string() + "-guid");
    const auto result = out / "result.json";
    REQUIRE(LaunchProcess(GLTF_COOKER_EXE, fixture.Args(out, result), true) == 0);

    const fs::path source = fixture.root / "materials/sand.material.json";
    AssetMeta meta = *ReadAssetMeta(AssetMetaPath(source));
    meta.guid = AssetId{ meta.guid.value ^ 0x10 };
    REQUIRE(WriteAssetMeta(AssetMetaPath(source), meta));
    REQUIRE(LaunchProcess(GLTF_COOKER_EXE, fixture.Args(out, result), true) == 0);

    const auto manifest = AssetManifest::LoadFile(out / "manifest.blob");
    REQUIRE(manifest);
    CHECK(test::PathId(*manifest, "assets/materials/sand.material.json") == meta.guid);
}
