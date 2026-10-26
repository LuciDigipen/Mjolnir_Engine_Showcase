/**
 * @file RtInstanceLayout.tests.cpp
 * @author Sumin Park
 * @brief Tests for renderer/RtInstanceInfo.
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

#include <renderer/RtInstanceInfo.h>

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace
{
    struct Field
    {
        const char *mName;
        std::size_t mOffset;
        std::size_t mSize;
    };

    constexpr std::array<Field, 15> kFields{ {
        { "vertexAddress", offsetof(mir::RtInstanceInfo, vertexAddress), sizeof(VkDeviceAddress) },
        { "indexAddress", offsetof(mir::RtInstanceInfo, indexAddress), sizeof(VkDeviceAddress) },
        { "tint", offsetof(mir::RtInstanceInfo, tint), sizeof(glm::vec4) },
        { "normalCol0", offsetof(mir::RtInstanceInfo, normalCol0), sizeof(glm::vec4) },
        { "normalCol1", offsetof(mir::RtInstanceInfo, normalCol1), sizeof(glm::vec4) },
        { "normalCol2", offsetof(mir::RtInstanceInfo, normalCol2), sizeof(glm::vec4) },
        { "flags", offsetof(mir::RtInstanceInfo, flags), sizeof(uint32_t) },
        { "baseColorTexture", offsetof(mir::RtInstanceInfo, baseColorTexture), sizeof(uint32_t) },
        { "baseColorSampler", offsetof(mir::RtInstanceInfo, baseColorSampler), sizeof(uint32_t) },
        { "materialRecord", offsetof(mir::RtInstanceInfo, materialRecord), sizeof(uint32_t) },
        { "alphaCutoff", offsetof(mir::RtInstanceInfo, alphaCutoff), sizeof(float) },
        { "pad0", offsetof(mir::RtInstanceInfo, pad0), sizeof(float) },
        { "pad1", offsetof(mir::RtInstanceInfo, pad1), sizeof(float) },
        { "pad2", offsetof(mir::RtInstanceInfo, pad2), sizeof(float) },
        { "prevModel", offsetof(mir::RtInstanceInfo, prevModel), sizeof(glm::mat4) } } };

    int RunSlangc(const std::string &commandLine, const std::filesystem::path &executable)
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

    std::vector<uint32_t> ReadSpirv(const std::filesystem::path &path)
    {
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        if (!in)
            return {};
        const std::streamsize bytes = in.tellg();
        in.seekg(0);
        std::vector<uint32_t> words(static_cast<std::size_t>(bytes) / sizeof(uint32_t));
        in.read(reinterpret_cast<char *>(words.data()), static_cast<std::streamsize>(words.size() * sizeof(uint32_t)));
        return words;
    }

    std::vector<uint32_t> ArrayStrides(const std::vector<uint32_t> &words)
    {
        constexpr uint16_t kOpDecorate = 71;
        constexpr uint32_t kArrayStride = 6;
        std::vector<uint32_t> strides;
        for (std::size_t i = 5; i < words.size();)
        {
            const uint16_t count = static_cast<uint16_t>(words[i] >> 16);
            const uint16_t opcode = static_cast<uint16_t>(words[i] & 0xffff);
            if (count == 0)
                break;
            if (opcode == kOpDecorate && i + 3 < words.size() && words[i + 2] == kArrayStride)
                strides.push_back(words[i + 3]);
            i += count;
        }
        return strides;
    }
}

TEST_CASE("The CPU RtInstanceInfo matches the compiled Slang layout", "[renderer][material][rt]")
{
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / "mir_rt_instance_layout";
    std::filesystem::create_directories(directory);
    const std::filesystem::path reflectionPath = directory / "raytrace.reflection.json";
    const std::filesystem::path spirvPath = directory / "raytrace.closestHitMain.spv";
    std::filesystem::remove(reflectionPath);
    std::filesystem::remove(spirvPath);

    const std::string command = "\"" + std::string(MIR_SLANGC_EXECUTABLE) + "\" \"" +
        std::string(MIR_RAYTRACE_SLANG) +
        "\" -target spirv -profile spirv_1_5 -entry closestHitMain -stage closesthit "
        "-fvk-use-entrypoint-name -matrix-layout-column-major -g -O0 -reflection-json \"" +
        reflectionPath.string() + "\" -o \"" + spirvPath.string() + "\"";
    REQUIRE(RunSlangc(command, MIR_SLANGC_EXECUTABLE) == 0);
    REQUIRE(std::filesystem::exists(reflectionPath));
    REQUIRE(std::filesystem::exists(spirvPath));

    std::ifstream reflectionFile(reflectionPath);
    nlohmann::json reflection;
    reflectionFile >> reflection;

    std::map<std::string, std::pair<std::size_t, std::size_t>> compiled;
    for (const nlohmann::json &parameter : reflection.at("parameters"))
    {
        if (parameter.at("name") != "gInstances")
            continue;

        for (const nlohmann::json &field : parameter.at("type").at("resultType").at("fields"))
        {
            const nlohmann::json &binding = field.at("binding");
            compiled.emplace(field.at("name").get<std::string>(),
                std::make_pair(binding.at("offset").get<std::size_t>(),
                    binding.at("size").get<std::size_t>()));
        }
    }

    REQUIRE(compiled.size() == kFields.size());
    std::size_t compiledSize = 0;
    for (const Field &field : kFields)
    {
        const auto found = compiled.find(field.mName);
        INFO("field " << field.mName);
        REQUIRE(found != compiled.end());
        CHECK(found->second.first == field.mOffset);
        CHECK(found->second.second == field.mSize);
        compiledSize = std::max(compiledSize, found->second.first + found->second.second);
    }
    CHECK(compiledSize == sizeof(mir::RtInstanceInfo));

    const std::vector<uint32_t> words = ReadSpirv(spirvPath);
    REQUIRE(words.size() >= 5);
    CHECK(words[0] == 0x07230203);
    const std::vector<uint32_t> strides = ArrayStrides(words);
    CHECK(std::find(strides.begin(), strides.end(), sizeof(mir::RtInstanceInfo)) != strides.end());
}
