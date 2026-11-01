/**
 * @file IdMigration.tests.cpp
 * @author sumin.park
 * @brief Tests for the one-off move from import.json files to .meta guids.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "../GltfImporter.h"
#include "../IdMigration.h"

#include <assets/AssetBlob.h>
#include <assets/AssetMeta.h>
#include <assets/ModelAsset.h>
#include <core/fs/FileIO.h>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>

using namespace mir;
namespace fs = std::filesystem;
using Json = nlohmann::json;

namespace
{
    std::string Hex(AssetId id) { return std::format("{:016x}", id.value); }

    // A game folder with a glTF that still has an old import.json, and a scene that stores its ids
    struct OldGame
    {
        fs::path root, source, data;
        Json oldSidecar;
        AssetId oldModel, oldClip;
        std::string clipKey;

        OldGame()
        {
            root = fs::current_path() /
                   ("migrate-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            source = root / "assets";
            data = root / "scenes";
            fs::create_directories(source);
            fs::create_directories(data / "entities");
            for (const auto &entry : fs::directory_iterator(GLTF_FIXTURE_DIR))
                if (entry.is_regular_file() && !IsAssetToolingFile(entry.path()))
                    fs::copy_file(entry.path(), source / entry.path().filename());

            // the part keys an import hands out, which an old import.json held in the same shape
            AssetMeta scratch;
            scratch.guid = AssetId{ 0x1111 };
            const cook::GltfImport imported = cook::ImportGltf(source / "flag.gltf", "games/G/assets/flag.gltf",
                { source }, scratch);
            const Json state = Json::parse(imported.importer);
            oldSidecar = { { "version", 2 }, { "next", state.at("next") }, { "objects", state.at("objects") },
                { "identityPath", "games/G/assets/flag.gltf" },
                { "aliases", { "games/G/assets/flag.gltf", "games/G/assets/moved/flag.gltf" } } };

            for (const auto &[match, number] : state.at("objects").items())
                if (match.starts_with("animation:"))
                    clipKey = "animation/" + number.get<std::string>();
            REQUIRE_FALSE(clipKey.empty());
            oldModel = MakeAssetId("games/G/assets/flag.gltf");
            oldClip = MakeAssetId("games/G/assets/flag.gltf#" + clipKey);

            std::ofstream(source / "flag.gltf.import.json") << oldSidecar.dump(2);
            WriteScene(Json{ { "modelAsset", Hex(oldModel) }, { "animationAsset", Hex(oldClip) },
                { "aliasAsset", Hex(MakeAssetId("games/G/assets/moved/flag.gltf")) },
                { "untouched", "00000000deadbeef" }, { "name", "Duck" } });
        }

        ~OldGame()
        {
            std::error_code ec;
            fs::remove_all(root, ec);
        }

        void WriteScene(const Json &fields) const
        {
            Json entity = { { "id", 1 }, { "components", Json::array({ { { "type", "ModelPlayer" }, { "fields", fields } } }) } };
            std::ofstream(data / "entities" / "1.json") << entity.dump(2);
        }

        Json Scene() const
        {
            std::ifstream in(data / "entities" / "1.json");
            return Json::parse(in).at("components").at(0).at("fields");
        }

        cook::IdMigrationOptions Options(bool apply) const
        {
            cook::IdMigrationOptions options;
            options.roots = { { source, "games/G/assets" } };
            options.dataDirs = { data };
            options.apply = apply;
            return options;
        }
    };
}

TEST_CASE("A dry run reports and changes nothing", "[cooker][migrate]")
{
    const OldGame game;
    const Json before = game.Scene();
    cook::MigrateIds(game.Options(false));

    CHECK(game.Scene() == before);
    CHECK(fs::exists(game.source / "flag.gltf.import.json"));
    CHECK_FALSE(fs::exists(AssetMetaPath(game.source / "flag.gltf")));
}

TEST_CASE("Applying gives the model a guid, carries its part keys and rewrites the stored ids", "[cooker][migrate]")
{
    const OldGame game;
    cook::MigrateIds(game.Options(true));

    const std::optional<AssetMeta> meta = ReadAssetMeta(AssetMetaPath(game.source / "flag.gltf"));
    REQUIRE(meta.has_value());
    CHECK_FALSE(meta->guid.IsNull());
    CHECK_FALSE(fs::exists(game.source / "flag.gltf.import.json"));

    const Json state = Json::parse(meta->importer);
    CHECK(state.at("objects") == game.oldSidecar.at("objects"));
    CHECK(state.at("next") == game.oldSidecar.at("next"));

    // the model, an alias of it and a clip now hold the new ids, and other values are left alone
    const Json scene = game.Scene();
    CHECK(scene.at("modelAsset") == Hex(meta->guid));
    CHECK(scene.at("aliasAsset") == Hex(meta->guid));
    CHECK(scene.at("animationAsset") == Hex(MakeChildAssetId(meta->guid, game.clipKey)));
    CHECK(scene.at("untouched") == "00000000deadbeef");
    CHECK(scene.at("name") == "Duck");
}

TEST_CASE("The ids the importer then hands out are the ones the scene was rewritten to", "[cooker][migrate]")
{
    const OldGame game;
    cook::MigrateIds(game.Options(true));
    const std::optional<AssetMeta> meta = ReadAssetMeta(AssetMetaPath(game.source / "flag.gltf"));
    REQUIRE(meta.has_value());

    const cook::GltfImport imported = cook::ImportGltf(game.source / "flag.gltf", "games/G/assets/flag.gltf",
        { game.source }, *meta);
    CHECK(imported.modelId.value == meta->guid.value);
    bool foundClip = false;
    for (const auto &blob : imported.blobs)
        foundClip = foundClip || blob.id.value == MakeChildAssetId(meta->guid, game.clipKey).value;
    CHECK(foundClip);
}

TEST_CASE("Applying twice changes nothing the second time", "[cooker][migrate]")
{
    const OldGame game;
    cook::MigrateIds(game.Options(true));
    const Json once = game.Scene();
    const std::optional<AssetMeta> meta = ReadAssetMeta(AssetMetaPath(game.source / "flag.gltf"));
    REQUIRE(meta.has_value());

    cook::MigrateIds(game.Options(true));
    CHECK(game.Scene() == once);
    CHECK(ReadAssetMeta(AssetMetaPath(game.source / "flag.gltf"))->guid == meta->guid);
}

TEST_CASE("An import.json with no source file is left alone", "[cooker][migrate]")
{
    const OldGame game;
    fs::remove(game.source / "flag.gltf");
    cook::MigrateIds(game.Options(true));
    CHECK(fs::exists(game.source / "flag.gltf.import.json"));
}

TEST_CASE("An import.json this tool cannot read is an error", "[cooker][migrate]")
{
    const OldGame game;
    std::ofstream(game.source / "flag.gltf.import.json") << R"({"version":2})";
    CHECK_THROWS_AS(cook::MigrateIds(game.Options(true)), std::runtime_error);
}
