/**
 * @file SurfaceMaterialLayout.tests.cpp
 * @author Jackey Cen
 * @brief Tests for the compiled surface material layout and shader manifest.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <renderer/MaterialReflection.h>
#include <renderer/SurfaceMaterialTexture.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

namespace
{
    int Run(const std::string &commandLine, const std::filesystem::path &executable)
    {
#if defined(_WIN32)
        const std::wstring wideCommand(commandLine.begin(), commandLine.end());
        const std::wstring wideExecutable = executable.wstring();
        std::vector<wchar_t> buffer(wideCommand.begin(), wideCommand.end());
        buffer.push_back(L'\0');

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(wideExecutable.c_str(), buffer.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process))
            return -1;

        WaitForSingleObject(process.hProcess, INFINITE);
        DWORD exitCode = 1;
        GetExitCodeProcess(process.hProcess, &exitCode);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return static_cast<int>(exitCode);
#else
        return std::system((commandLine + " > /dev/null 2>&1").c_str());
#endif
    }

    std::string Quote(const std::filesystem::path &path)
    {
        return "\"" + path.string() + "\"";
    }
}

TEST_CASE("The default surface record matches the compiled Slang layout", "[renderer][material][reflection]")
{
    const std::filesystem::path directory =
        std::filesystem::path(MIR_RENDERER_TEST_DIR) / "surface_material_layout";
    std::filesystem::create_directories(directory);
    const std::filesystem::path reflectionPath = directory / "default.layout.reflection.json";
    const std::filesystem::path spirvPath = directory / "default.layout.spv";
    std::filesystem::remove(reflectionPath);
    std::filesystem::remove(spirvPath);

    const std::string command = Quote(MIR_SLANGC_EXECUTABLE) + " " + Quote(MIR_SURFACE_LAYOUT_SLANG) + " " +
        Quote(MIR_DEFAULT_MATERIAL_SLANG) +
        " -target spirv -profile spirv_1_5 -entry layoutMain -stage compute -specialize DefaultMaterial "
        "-fvk-use-entrypoint-name -matrix-layout-column-major -fvk-use-gl-layout -g -O0 -reflection-json " +
        Quote(reflectionPath) + " -o " + Quote(spirvPath);
    REQUIRE(Run(command, MIR_SLANGC_EXECUTABLE) == 0);
    REQUIRE(std::filesystem::exists(reflectionPath));

    std::ifstream reflectionFile(reflectionPath);
    const std::string reflectionJson{ std::istreambuf_iterator<char>(reflectionFile),
        std::istreambuf_iterator<char>() };
    std::string error;
    const auto layout = mir::ParseMaterialLayout(reflectionJson, &error);
    INFO(error);
    REQUIRE(layout.has_value());
    REQUIRE(layout->fields.size() == 2);
    CHECK(layout->fields[0].name == "albedoRoughness");
    CHECK(layout->fields[0].offset == 0);
    CHECK(layout->fields[0].size == 16);
    CHECK(layout->fields[1].name == "surfaceParams");
    CHECK(layout->fields[1].offset == 16);
    CHECK(layout->fields[1].size == 16);
    CHECK(layout->size == 32);

    std::ifstream materialFile(MIR_DEFAULT_MATERIAL_JSON);
    nlohmann::json material;
    materialFile >> material;
    const auto record = mir::PackMaterialRecord(*layout, material.at("parameters").dump(), &error);
    INFO(error);
    REQUIRE(record.has_value());
    REQUIRE(record->size() == 32);
    float roughness = 0.0f;
    std::memcpy(&roughness, record->data() + 12, sizeof(roughness));
    CHECK(roughness == 0.4924790605f);
}

TEST_CASE("The terrain sand material reflects macro and parallax fields", "[renderer][material][reflection]")
{
    const std::filesystem::path directory =
        std::filesystem::path(MIR_RENDERER_TEST_DIR) / "terrain_material_layout";
    std::filesystem::create_directories(directory);
    const std::filesystem::path reflectionPath = directory / "icelandicSand.layout.reflection.json";
    const std::filesystem::path spirvPath = directory / "icelandicSand.layout.spv";
    std::filesystem::remove(reflectionPath);
    std::filesystem::remove(spirvPath);

    const std::string common = " -target spirv -profile spirv_1_5 -fvk-use-entrypoint-name "
        "-matrix-layout-column-major -fvk-use-gl-layout -g -O0 ";
    const std::string layoutCommand = Quote(MIR_SLANGC_EXECUTABLE) + " " + Quote(MIR_SURFACE_LAYOUT_SLANG) +
        " " + Quote(MIR_TERRAIN_MATERIAL_SLANG) + common +
        "-entry layoutMain -stage compute -specialize IcelandicSandMaterial -reflection-json " +
        Quote(reflectionPath) + " -o " + Quote(spirvPath);
    REQUIRE(Run(layoutCommand, MIR_SLANGC_EXECUTABLE) == 0);

    const std::string fragmentCommand = Quote(MIR_SLANGC_EXECUTABLE) + " " + Quote(MIR_TERRAIN_FORWARD_SLANG) +
        " " + Quote(MIR_TERRAIN_MATERIAL_SLANG) + common +
        "-entry fragmentMain -stage fragment -specialize IcelandicSandMaterial -o " +
        Quote(directory / "icelandicSand.fragmentMain.spv");
    REQUIRE(Run(fragmentCommand, MIR_SLANGC_EXECUTABLE) == 0);

    const std::string rayCommand = Quote(MIR_SLANGC_EXECUTABLE) + " " + Quote(MIR_TERRAIN_RT_SLANG) +
        " " + Quote(MIR_TERRAIN_MATERIAL_SLANG) + common +
        "-entry closestHitSurface -stage closesthit -specialize IcelandicSandMaterial -o " +
        Quote(directory / "icelandicSand.closestHitSurface.spv");
    REQUIRE(Run(rayCommand, MIR_SLANGC_EXECUTABLE) == 0);

    std::ifstream reflectionFile(reflectionPath);
    const std::string reflectionJson{ std::istreambuf_iterator<char>(reflectionFile),
        std::istreambuf_iterator<char>() };
    std::string error;
    const auto layout = mir::ParseMaterialLayout(reflectionJson, &error);
    INFO(error);
    REQUIRE(layout.has_value());

    std::ifstream materialFile(MIR_TERRAIN_MATERIAL_JSON);
    nlohmann::json material;
    materialFile >> material;
    nlohmann::json parameters = material.at("parameters");
    for (const mir::MaterialFieldLayout &field : layout->fields)
        if (field.kind == mir::MaterialFieldKind::TextureRef)
            parameters[field.name] = nlohmann::json{{"image", 0}, {"sampler", 0}};

    const auto record = mir::PackMaterialRecord(*layout, parameters.dump(), &error);
    INFO(error);
    REQUIRE(record.has_value());

    const auto macro = mir::SurfaceMaterialMacroParametersFromRecord(*layout, *record);
    REQUIRE(macro);
    REQUIRE(macro->periodMeters);
    CHECK(*macro->periodMeters == 64.0);
    CHECK(macro->strength == Catch::Approx(0.35));

    const auto parallax = mir::SurfaceMaterialParallaxParametersFromRecord(*layout, *record);
    REQUIRE(parallax);
    CHECK(parallax->enabled);
    CHECK_FALSE(parallax->rayTracedEnabled);
    CHECK(parallax->heightMeters == Catch::Approx(0.057));
    CHECK(parallax->reference == Catch::Approx(0.455));
    CHECK(parallax->steps == 24);
}

TEST_CASE("The shader manifest covers legacy and namespaced surface outputs", "[renderer][material][manifest]")
{
    std::ifstream manifestFile(MIR_SHADER_MANIFEST);
    REQUIRE(manifestFile);
    nlohmann::json manifest;
    manifestFile >> manifest;

    CHECK(manifest.at("version") == 1);
    CHECK(manifest.at("workingDirectory").is_string());
    CHECK(manifest.at("commonArguments").is_array());
    CHECK(manifest.at("frontendTemplates").is_array());
    REQUIRE(manifest.at("outputs").is_array());

    const std::string defaultStem =
        mir::MaterialShaderStem("default", "DefaultMaterial", "assets/materials/default.slang", "mesh");
    const auto &common = manifest.at("commonArguments");
    if (std::string(MIR_BUILD_TYPE) == "Debug")
    {
        CHECK(std::find(common.begin(), common.end(), "-g") != common.end());
        CHECK(std::find(common.begin(), common.end(), "-O0") != common.end());
    }
    else
    {
        CHECK(std::find(common.begin(), common.end(), "-O3") != common.end());
        CHECK(std::find(common.begin(), common.end(), "-g") == common.end());
    }

    std::set<std::string> outputs;
    bool legacyDefault = false;
    bool surfaceDefault = false;
    for (const nlohmann::json &output : manifest.at("outputs"))
    {
        CHECK(output.at("sources").is_array());
        CHECK_FALSE(output.at("sources").empty());
        CHECK(output.at("entry").is_string());
        CHECK(output.at("stage").is_string());
        CHECK(output.at("output").is_string());
        CHECK(output.at("reflection").is_string());
        CHECK(output.at("depfile").is_string());
        CHECK(outputs.insert(output.at("output").get<std::string>()).second);

        const nlohmann::json &arguments = output.at("arguments");
        REQUIRE(arguments.is_array());
        CHECK(arguments.at(0) == manifest.at("slangc"));
        CHECK(std::find(arguments.begin(), arguments.end(), output.at("entry")) != arguments.end());
        CHECK(std::find(arguments.begin(), arguments.end(), output.at("stage")) != arguments.end());
        CHECK(std::find(arguments.begin(), arguments.end(), output.at("reflection")) != arguments.end());
        CHECK(std::find(arguments.begin(), arguments.end(), output.at("depfile")) != arguments.end());

        const std::string path = output.at("output").get<std::string>();
        legacyDefault = legacyDefault || path.find("default.vertexMain.spv") != std::string::npos;
        surfaceDefault = surfaceDefault || path.find(defaultStem + ".fragmentMain.spv") != std::string::npos;
    }
    CHECK(legacyDefault);
    CHECK(surfaceDefault);
}

TEST_CASE("The runtime surface catalog names unique assets and valid shared shader outputs", "[renderer][material][manifest]")
{
    std::ifstream catalogFile(MIR_SURFACE_CATALOG);
    REQUIRE(catalogFile);
    nlohmann::json catalog;
    catalogFile >> catalog;
    CHECK(catalog.at("version") == 1);
    REQUIRE(catalog.at("materials").is_array());
    CHECK_FALSE(catalog.at("materials").empty());

    const std::filesystem::path shaderDir = std::filesystem::path(MIR_SURFACE_CATALOG).parent_path();
    std::ifstream manifestFile(MIR_SHADER_MANIFEST);
    REQUIRE(manifestFile);
    nlohmann::json manifest;
    manifestFile >> manifest;
    std::set<std::filesystem::path> compiledOutputs;
    for (const nlohmann::json &output : manifest.at("outputs"))
        compiledOutputs.insert(std::filesystem::path(output.at("output").get<std::string>()).lexically_normal());

    std::set<std::string> assets;
    bool defaultMaterial = false;
    for (const nlohmann::json &material : catalog.at("materials"))
    {
        const std::string stem = material.at("stem").get<std::string>();
        CHECK(assets.insert(material.at("asset").get<std::string>() + "|" + material.at("frontend").get<std::string>()).second);
        for (const char *entry : { "vertexMain", "fragmentMain", "closestHitSurface", "layout" })
        {
            const auto outputPath = shaderDir / (stem + "." + entry + ".spv");
            INFO(outputPath.string());
            CHECK(compiledOutputs.contains(outputPath.lexically_normal()));
        }
        CHECK((material.at("frontend") == "mesh" || material.at("frontend") == "terrain"));
        CHECK(material.at("parameters").is_object());
        CHECK(material.at("reflection").get<std::string>().find('/') == std::string::npos);
        defaultMaterial = defaultMaterial ||
            material.at("asset") == "assets/materials/default.material.json";
    }
    CHECK(defaultMaterial);

    for (const nlohmann::json &material : catalog.at("materials"))
    {
        std::ifstream reflectionFile(shaderDir / material.at("reflection").get<std::string>());
        REQUIRE(reflectionFile);
        const std::string reflection{ std::istreambuf_iterator<char>(reflectionFile),
            std::istreambuf_iterator<char>() };
        std::string error;
        const auto layout = mir::ParseMaterialLayout(reflection, &error);
        INFO(error);
        REQUIRE(layout.has_value());
        nlohmann::json parameters = material.at("parameters");
        for (const mir::MaterialFieldLayout &field : layout->fields)
            if (field.kind == mir::MaterialFieldKind::TextureRef)
                parameters[field.name] = nlohmann::json{{"image", 0}, {"sampler", 0}};
        const auto record = mir::PackMaterialRecord(*layout, parameters.dump(), &error);
        INFO(error);
        REQUIRE(record.has_value());
        CHECK(record->size() == layout->size);
    }
}
