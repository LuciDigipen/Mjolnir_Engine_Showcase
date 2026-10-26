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

#include <renderer/Material.h>
#include <renderer/MaterialReflection.h>

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
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
        std::filesystem::temp_directory_path() / "mir_surface_material_layout";
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
    CHECK(layout->fields[0].offset == offsetof(mir::DefaultSurfaceMaterial, albedoRoughness));
    CHECK(layout->fields[1].name == "surfaceParams");
    CHECK(layout->fields[1].offset == offsetof(mir::DefaultSurfaceMaterial, surfaceParams));
    CHECK(layout->size == sizeof(mir::DefaultSurfaceMaterial));

    std::ifstream materialFile(MIR_DEFAULT_MATERIAL_JSON);
    nlohmann::json material;
    materialFile >> material;
    const auto record = mir::PackMaterialRecord(*layout, material.at("parameters").dump(), &error);
    INFO(error);
    REQUIRE(record.has_value());
    const mir::DefaultSurfaceMaterial expected{};
    CHECK(std::memcmp(record->data(), &expected, sizeof(expected)) == 0);
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

        const std::string path = output.at("output").get<std::string>();
        legacyDefault = legacyDefault || path.find("default.vertexMain.spv") != std::string::npos;
        surfaceDefault = surfaceDefault ||
            path.find("default_DefaultMaterial.fragmentMain.spv") != std::string::npos;
    }
    CHECK(legacyDefault);
    CHECK(surfaceDefault);
}
