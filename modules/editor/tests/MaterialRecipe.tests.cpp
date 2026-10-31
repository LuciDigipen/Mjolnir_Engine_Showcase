/**
 * @file MaterialRecipe.tests.cpp
 * @author Jackey Cen
 * @brief Tests for editor/MaterialRecipe.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <editor/MaterialRecipe.h>

#include <core/fs/FileIO.h>
#include <serial/JsonFile.h>

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace
{
    const std::string &ProcessToken()
    {
        static const std::string token = []
            {
                std::random_device rd;
                std::ostringstream out;
                out << std::hex << ((static_cast<std::uint64_t>(rd()) << 32) ^ rd());
                return out.str();
            }();
        return token;
    }

    std::filesystem::path MakeScratchFile()
    {
        static std::atomic<int> counter{ 0 };
        return std::filesystem::temp_directory_path() /
            ("mjolnir_material_recipe_test_" + ProcessToken() + "_" + std::to_string(counter.fetch_add(1)) + ".json");
    }

    constexpr const char *kRecipe = R"({
        "version": 1,
        "shader": { "file": "materials/test.slang", "type": "TestMaterial" },
        "frontends": ["terrain"],
        "parameters": { "tileSizeMeters": 2.0, "unknownParameter": 9.0 },
        "textures": {
            "baseColor": { "path": "textures/a.png", "colorSpace": "srgb", "future": 3 },
            "normal": { "path": "textures/n.png", "colorSpace": "linear" }
        },
        "resolution": 1024,
        "futureRoot": { "kept": true }
    })";
}

TEST_CASE("A material recipe loads its reflected fields and authored textures", "[editor][material]")
{
    const std::filesystem::path path = MakeScratchFile();
    REQUIRE(mir::WriteText(path, kRecipe));

    std::string error;
    const std::optional<mir::MaterialRecipe> recipe = mir::LoadMaterialRecipe(path, &error);
    INFO(error);
    REQUIRE(recipe);
    CHECK(recipe->shaderType == "TestMaterial");
    CHECK(recipe->frontends == std::vector<std::string>{"terrain"});
    CHECK(recipe->resolution == 1024);
    CHECK(recipe->textures.size() == 2);
    CHECK(recipe->textures[0].role == "baseColor");
    CHECK(recipe->textures[0].colorSpace == "srgb");

    const nlohmann::json parameters = nlohmann::json::parse(recipe->parametersJson);
    CHECK(parameters.at("tileSizeMeters") == 2.0);
    CHECK(parameters.at("unknownParameter") == 9.0);

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

TEST_CASE("A material save writes only the named fields", "[editor][material]")
{
    const std::filesystem::path path = MakeScratchFile();
    REQUIRE(mir::WriteText(path, kRecipe));

    std::string error;
    REQUIRE(mir::SaveMaterialRecipe(path, R"({"tileSizeMeters": 4.0})",
        {{"baseColor", "textures/b.png"}}, &error));
    INFO(error);

    const std::optional<nlohmann::json> saved = mir::ReadJsonFile(path);
    REQUIRE(saved);
    CHECK(saved->at("parameters").at("tileSizeMeters") == 4.0);
    CHECK(saved->at("parameters").at("unknownParameter") == 9.0);
    CHECK(saved->at("parameters").contains("tileSizeMeters"));
    CHECK_FALSE(saved->at("parameters").contains("materialPath"));
    CHECK(saved->at("textures").at("baseColor").at("path") == "textures/b.png");
    CHECK(saved->at("textures").at("baseColor").at("colorSpace") == "srgb");
    CHECK(saved->at("textures").at("baseColor").at("future") == 3);
    CHECK(saved->at("textures").at("normal").at("path") == "textures/n.png");
    CHECK(saved->at("resolution") == 1024);
    CHECK(saved->at("futureRoot").at("kept") == true);
    CHECK(saved->at("shader").at("type") == "TestMaterial");

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

TEST_CASE("A material save refuses a missing source without creating it", "[editor][material]")
{
    const std::filesystem::path path = MakeScratchFile();
    std::error_code ignored;
    std::filesystem::remove(path, ignored);

    std::string error;
    CHECK_FALSE(mir::SaveMaterialRecipe(path, R"({"tileSizeMeters": 4.0})", {}, &error));
    CHECK_FALSE(std::filesystem::exists(path));
    CHECK(error.find("no longer exists") != std::string::npos);
}

TEST_CASE("A material save refuses malformed JSON and unknown texture roles", "[editor][material]")
{
    const std::filesystem::path malformed = MakeScratchFile();
    REQUIRE(mir::WriteText(malformed, R"({"version": 1, "parameters": )"));
    std::string error;
    CHECK_FALSE(mir::SaveMaterialRecipe(malformed, R"({"a": 1.0})", {}, &error));
    const std::optional<std::string> unchanged = mir::ReadText(malformed);
    REQUIRE(unchanged);
    CHECK(*unchanged == R"({"version": 1, "parameters": )");
    std::error_code ignored;
    std::filesystem::remove(malformed, ignored);

    const std::filesystem::path path = MakeScratchFile();
    REQUIRE(mir::WriteText(path, kRecipe));
    CHECK_FALSE(mir::SaveMaterialRecipe(path, R"({"tileSizeMeters": 4.0})",
        {{"roughness", "textures/r.png"}}, &error));
    CHECK(error.find("not authored") != std::string::npos);

    const std::optional<nlohmann::json> saved = mir::ReadJsonFile(path);
    REQUIRE(saved);
    CHECK(saved->at("parameters").at("tileSizeMeters") == 2.0);
    std::filesystem::remove(path, ignored);
}

TEST_CASE("A logical material maps to a source file only under the project root", "[editor][material]")
{
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "mjolnir_material_source";
    const std::optional<std::filesystem::path> file =
        mir::MaterialSourceFile(root, "games/Test/assets", "games\\Test\\assets\\materials\\..\\materials\\a.material.json");
    REQUIRE(file);
    CHECK(file->lexically_relative(root) == std::filesystem::path("materials/a.material.json"));

    CHECK_FALSE(mir::MaterialSourceFile(root, "games/Test/assets", "games/Test/assets/../other/a.material.json"));
    CHECK_FALSE(mir::MaterialSourceFile(root, "games/Test/assets", "games/Other/assets/a.material.json"));
    CHECK_FALSE(mir::MaterialSourceFile(root, "games/Test/assets", "/games/Test/assets/a.material.json"));
}
