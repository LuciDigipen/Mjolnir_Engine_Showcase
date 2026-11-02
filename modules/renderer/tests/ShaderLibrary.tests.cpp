/**
 * @file ShaderLibrary.tests.cpp
 * @author Jackey Cen
 * @brief Tests for renderer/ShaderLibrary.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer/ShaderGeneration.h>
#include <renderer/ShaderLibrary.h>

#include <catch2/catch_test_macros.hpp>

#include <core/fs/FileIO.h>
#include <core/platform/Process.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace
{
    int RunCommand(const std::string &commandLine, const std::filesystem::path &executable)
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
        WaitForSingleObject(process.hProcess, 60000);
        DWORD exitCode = 1;
        GetExitCodeProcess(process.hProcess, &exitCode);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return static_cast<int>(exitCode);
#else
        return std::system((commandLine + " > /dev/null 2>&1").c_str());
#endif
    }
}

namespace
{
    std::filesystem::path TestDirectory()
    {
        static std::atomic<unsigned> counter{ 0 };
        const std::filesystem::path directory = std::filesystem::temp_directory_path() /
            ("mir_shader_library_tests_" + std::to_string(counter.fetch_add(1)) + "_" +
                std::to_string(mir::CurrentProcessId()) + "_" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::remove_all(directory);
        std::filesystem::create_directories(directory);
        return directory;
    }

    std::vector<std::byte> Words(std::initializer_list<uint32_t> words)
    {
        std::vector<std::byte> bytes(words.size() * sizeof(uint32_t));
        std::memcpy(bytes.data(), words.begin(), bytes.size());
        return bytes;
    }

    const char *kReflection = R"({
        "parameters": [
            { "name": "camera", "binding": {"kind": "descriptorTableSlot", "space": 1, "index": 0},
              "type": {"kind": "constantBuffer", "elementType": {"kind": "struct", "fields": [
                {"name": "viewProj", "type": {"kind": "matrix", "rowCount": 4, "columnCount": 4,
                 "elementType": {"kind": "scalar", "scalarType": "float32"}},
                 "binding": {"kind": "uniform", "offset": 0, "size": 64}}]},
                "elementVarLayout": {"binding": {"kind": "uniform", "offset": 0, "size": 64}}} },
            { "name": "records", "binding": {"kind": "descriptorTableSlot", "space": 4, "index": 0},
              "type": {"kind": "resource", "baseShape": "byteAddressBuffer"} },
            { "name": "images", "binding": {"kind": "descriptorTableSlot", "space": 4, "index": 1},
              "type": {"kind": "array", "elementCount": 8,
                "elementType": {"kind": "resource", "baseShape": "texture2D"}} },
            { "name": "pushData", "binding": {"kind": "pushConstantBuffer", "index": 0},
              "type": {"kind": "constantBuffer", "elementVarLayout": {"binding": {"kind": "uniform",
                "offset": 0, "size": 128}}} }
        ],
        "entryPoints": [{
            "name": "fragmentMain", "stage": "fragment",
            "parameters": [
                { "name": "input", "binding": {"kind": "varyingInput", "index": 0, "count": 1},
                  "type": {"kind": "struct", "fields": [
                    {"name": "worldPos", "binding": {"kind": "varyingInput", "index": 0},
                     "semanticName": "TEXCOORD",
                     "type": {"kind": "vector", "elementCount": 3,
                              "elementType": {"kind": "scalar", "scalarType": "float32"}}}]}},
                { "name": "pushData", "binding": {"kind": "pushConstantBuffer", "index": 0},
                  "type": {"kind": "constantBuffer", "elementVarLayout": {"binding": {"kind": "uniform",
                    "offset": 0, "size": 128}}} }
            ],
            "result": {"binding": {"kind": "varyingOutput", "index": 0},
                       "type": {"kind": "struct", "fields": [
                         {"name": "color", "binding": {"kind": "varyingOutput", "index": 0},
                          "semanticName": "SV_TARGET",
                          "type": {"kind": "vector", "elementCount": 4,
                                   "elementType": {"kind": "scalar", "scalarType": "float32"}}}]}},
            "bindings": [
                {"name": "camera", "binding": {"kind": "descriptorTableSlot", "space": 1, "index": 0, "used": 1}},
                {"name": "records", "binding": {"kind": "descriptorTableSlot", "space": 4, "index": 0, "used": 1}},
                {"name": "images", "binding": {"kind": "descriptorTableSlot", "space": 4, "index": 1, "used": 1}}
            ]
        }]
    })";
}

TEST_CASE("A depfile keeps spaced Windows paths and shared includes", "[renderer][shader][depfile]")
{
    const std::string depfile =
        "C:\\build dir\\out.spv: C:\\source\\ dir\\SurfaceForward.slang C:\\source\\ dir\\shared\\ include.slang "
        "C:/source/plain.slang \\\n C:\\source\\ dir\\shared\\ include.slang\n";
    const auto dependencies = mir::ParseShaderDepfile(depfile);
    REQUIRE(dependencies.has_value());
    REQUIRE(dependencies->size() == 3);
    CHECK((*dependencies)[0] == "C:\\source dir\\SurfaceForward.slang");
    CHECK((*dependencies)[1] == "C:\\source dir\\shared include.slang");
    CHECK((*dependencies)[2] == "C:/source/plain.slang");

    std::string error;
    CHECK_FALSE(mir::ParseShaderDepfile("no rule here\n", &error).has_value());
    CHECK_FALSE(error.empty());
}

TEST_CASE("SPIR-V modules need the magic number and whole words", "[renderer][shader][spirv]")
{
    std::string error;
    CHECK(mir::ValidateSpirvModule(Words({ 0x07230203, 0x00010500, 0, 1, 0 }), &error));
    CHECK_FALSE(mir::ValidateSpirvModule(Words({ 0x03022307, 0x00010500, 0, 1, 0 }), &error));
    error.clear();
    CHECK_FALSE(mir::ValidateSpirvModule(std::span<const std::byte>(Words({ 0x07230203 }).data(), 3), &error));
    CHECK_FALSE(error.empty());
}

TEST_CASE("Reflection compatibility covers descriptors, push ranges and stage IO", "[renderer][shader][abi]")
{
    std::string error;
    const auto accepted = mir::ParseShaderReflection(kReflection, "fragment", &error);
    INFO(error);
    REQUIRE(accepted.has_value());
    CHECK(accepted->entryPoint == "fragmentMain");
    CHECK(accepted->stage == "fragment");
    REQUIRE(accepted->descriptors.size() == 3);
    CHECK(accepted->descriptors[0].set == 1);
    CHECK(accepted->descriptors[1].set == 4);
    CHECK(accepted->descriptors[2].count == 8);
    REQUIRE(accepted->pushConstants.size() == 1);
    CHECK(accepted->pushConstants[0].size == 128);
    REQUIRE(accepted->inputs.size() == 1);
    CHECK(accepted->inputs[0].semantic == "TEXCOORD");
    REQUIRE(accepted->outputs.size() == 1);
    CHECK(accepted->outputs[0].semantic == "SV_TARGET");

    const auto same = mir::ParseShaderReflection(kReflection, "fragment", &error);
    REQUIRE(same.has_value());
    const mir::ShaderCompatibility identical = mir::CompareShaderReflection(*accepted, *same);
    CHECK(identical.compatible);
    CHECK_FALSE(identical.restartRequired);

    std::string changedDescriptor = kReflection;
    changedDescriptor.replace(changedDescriptor.find("\"index\": 1"), 10, "\"index\": 7");
    const auto descriptorCandidate = mir::ParseShaderReflection(changedDescriptor, "fragment", &error);
    REQUIRE(descriptorCandidate.has_value());
    const mir::ShaderCompatibility descriptor = mir::CompareShaderReflection(*accepted, *descriptorCandidate);
    CHECK_FALSE(descriptor.compatible);
    CHECK(descriptor.restartRequired);
    REQUIRE_FALSE(descriptor.differences.empty());

    std::string changedStage = kReflection;
    changedStage.replace(changedStage.find("\"stage\": \"fragment\""), 19, "\"stage\": \"vertex\"");
    const auto stageCandidate = mir::ParseShaderReflection(changedStage, "vertex", &error);
    REQUIRE(stageCandidate.has_value());
    CHECK(mir::CompareShaderReflection(*accepted, *stageCandidate).restartRequired);
}

TEST_CASE("Material records migrate by name and reject a changed type", "[renderer][shader][migration]")
{
    mir::MaterialLayout accepted;
    accepted.fields.push_back(mir::MaterialFieldLayout{ "albedo", mir::MaterialFieldKind::Float4, 0, 16 });
    accepted.fields.push_back(mir::MaterialFieldLayout{ "roughness", mir::MaterialFieldKind::Float, 16, 4 });
    accepted.size = 32;

    mir::MaterialLayout candidate;
    candidate.fields.push_back(mir::MaterialFieldLayout{ "roughness", mir::MaterialFieldKind::Float, 0, 4 });
    candidate.fields.push_back(mir::MaterialFieldLayout{ "albedo", mir::MaterialFieldKind::Float4, 16, 16 });
    candidate.fields.push_back(mir::MaterialFieldLayout{ "emissive", mir::MaterialFieldKind::Float3, 32, 12 });
    candidate.size = 48;

    std::vector<std::byte> bytes(accepted.size);
    const float albedo[4]{ 0.25f, 0.5f, 0.75f, 1.0f };
    const float roughness = 0.6f;
    std::memcpy(bytes.data(), albedo, sizeof(albedo));
    std::memcpy(bytes.data() + 16, &roughness, sizeof(roughness));

    std::vector<std::byte> defaults(candidate.size);
    const float emissive[3]{ 1.0f, 2.0f, 3.0f };
    std::memcpy(defaults.data() + 32, emissive, sizeof(emissive));

    const mir::MaterialMigration migrated = mir::MigrateMaterialRecord(accepted, candidate, bytes, defaults);
    INFO(migrated.error);
    REQUIRE(migrated.ok);
    CHECK(migrated.recordChanged);
    REQUIRE(migrated.defaultedFields.size() == 1);
    CHECK(migrated.defaultedFields[0] == "emissive");
    REQUIRE(migrated.bytes.size() == candidate.size);
    float migratedRoughness = 0.0f;
    std::memcpy(&migratedRoughness, migrated.bytes.data(), sizeof(migratedRoughness));
    CHECK(migratedRoughness == 0.6f);
    float migratedAlbedo[4]{};
    std::memcpy(migratedAlbedo, migrated.bytes.data() + 16, sizeof(migratedAlbedo));
    CHECK(migratedAlbedo[1] == 0.5f);
    float migratedEmissive[3]{};
    std::memcpy(migratedEmissive, migrated.bytes.data() + 32, sizeof(migratedEmissive));
    CHECK(migratedEmissive[2] == 3.0f);

    mir::MaterialLayout changed = candidate;
    changed.fields[0].kind = mir::MaterialFieldKind::Float2;
    changed.fields[0].size = 8;
    const mir::MaterialMigration rejected = mir::MigrateMaterialRecord(accepted, changed, bytes, defaults);
    CHECK_FALSE(rejected.ok);
    CHECK(rejected.error.find("changed type") != std::string::npos);

    const mir::MaterialMigration firstBuild = mir::MigrateMaterialRecord({}, candidate, {}, defaults);
    REQUIRE(firstBuild.ok);
    CHECK(firstBuild.defaultedFields.size() == 3);
}

TEST_CASE("A shared include invalidates every dependent output and stale revisions cancel", "[renderer][shader][graph]")
{
    mir::ShaderDependencyIndex index;
    index.AddOutput("forward.spv");
    index.AddOutput("rt.spv");
    index.AddDependency("shared.slang", "forward.spv");
    index.AddDependency("shared.slang", "rt.spv");
    const mir::ShaderSourceFingerprint first{ 10, 100, true };
    const mir::ShaderSourceFingerprint second{ 11, 200, true };

    CHECK(index.Observe("shared.slang", first));
    const uint64_t revision = index.Revision();
    CHECK_FALSE(index.IsCurrent(revision - 1));
    CHECK(index.IsCurrent(revision));
    CHECK(index.Observe("shared.slang", second));
    CHECK(index.Revision() == revision + 1);
    CHECK_FALSE(index.IsCurrent(revision));

    const std::vector<std::string> changed{ "shared.slang" };
    const std::vector<std::string> affected = index.AffectedOutputs(changed);
    REQUIRE(affected.size() == 2);
    CHECK(affected[0] == "forward.spv");
    CHECK(affected[1] == "rt.spv");
}

TEST_CASE("Staged shader publication is all-or-none and refuses an older build", "[renderer][shader][files]")
{
    const std::filesystem::path directory = TestDirectory();
    const std::filesystem::path target = directory / "sample.spv";
    const std::string oldBytes = "old";

    {
        std::ofstream file(target, std::ios::binary);
        file << oldBytes;
    }

    const std::vector<std::byte> newBytes = Words({ 0x07230203, 0x00010500, 0, 1, 0 });
    mir::StagedShaderPublisher publisher(directory, 5, 500);
    std::string error;
    REQUIRE(publisher.Stage("sample.spv", newBytes, &error));
    REQUIRE(publisher.StageText("sample.spv.reflection.json", "{}", &error));
    CHECK(publisher.StagedNames().size() == 2);

    std::string before;
    {
        std::ifstream file(target, std::ios::binary);
        before.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }
    CHECK(before == oldBytes);
    CHECK_FALSE(std::filesystem::exists(directory / "sample.spv.stamp"));

    REQUIRE(publisher.Commit(&error));
    const auto stamp = mir::StagedShaderPublisher::ReadStamp(target);
    REQUIRE(stamp.has_value());
    CHECK(stamp->revision == 5);
    CHECK(stamp->sourceFingerprint == 500);

    mir::StagedShaderPublisher older(directory, 4, 400);
    REQUIRE(older.Stage("sample.spv", newBytes, &error));
    CHECK_FALSE(older.Commit(&error));
    CHECK(error.find("newer accepted") != std::string::npos);

    mir::StagedShaderPublisher newer(directory, 6, 600);
    REQUIRE(newer.Stage("sample.spv", newBytes, &error));
    REQUIRE(newer.Commit(&error));

    mir::StagedShaderPublisher discarded(directory, 7, 700);
    REQUIRE(discarded.Stage("sample.spv", newBytes, &error));
    discarded.Discard();
    CHECK_FALSE(std::filesystem::exists(directory / "sample.spv.tmp.7"));

    std::filesystem::remove_all(directory);
}

namespace
{
    std::string ReadTestFile(const std::filesystem::path &path)
    {
        std::ifstream file(path, std::ios::binary);
        REQUIRE(file);
        return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }

    std::optional<nlohmann::json> LoadManifestJson()
    {
        try
        {
            return nlohmann::json::parse(ReadTestFile(MIR_SHADER_MANIFEST));
        }
        catch (const std::exception &)
        {
            return std::nullopt;
        }
    }

    const nlohmann::json *FindOutput(const nlohmann::json &manifest, const std::string &name)
    {
        for (const nlohmann::json &output : manifest.at("outputs"))
        {
            if (std::filesystem::path(output.at("output").get<std::string>()).filename() == name)
                return &output;
        }
        return nullptr;
    }

    std::string ReflectionText(const nlohmann::json &output)
    {
        const std::filesystem::path shaderDir = std::filesystem::path(MIR_SHADER_MANIFEST).parent_path();
        return ReadTestFile(shaderDir / std::filesystem::path(output.at("reflection").get<std::string>()).filename());
    }

    void MutateFirstPushSize(nlohmann::json &root)
    {
        for (nlohmann::json &parameter : root.at("parameters"))
        {
            if (!parameter.contains("binding") || parameter.at("binding").value("kind", "") != "pushConstantBuffer")
                continue;
            nlohmann::json &layout = parameter.at("type").at("elementVarLayout").at("binding");
            layout["size"] = layout.at("size").get<uint32_t>() + 16;
            return;
        }
        FAIL("the compiled reflection has no global push constant");
    }

    void MutateFirstUniformOffset(nlohmann::json &root)
    {
        for (nlohmann::json &parameter : root.at("parameters"))
        {
            if (!parameter.contains("type") || !parameter.at("type").contains("elementType") ||
                !parameter.at("type").at("elementType").contains("fields"))
                continue;
            for (nlohmann::json &field : parameter.at("type").at("elementType").at("fields"))
            {
                nlohmann::json &binding = field.at("binding");
                if (binding.value("kind", "") == "uniform" && binding.value("size", 0u) > 0)
                {
                    binding["offset"] = binding.at("offset").get<uint32_t>() + 16;
                    return;
                }
            }
        }
        FAIL("the compiled reflection has no uniform member");
    }

    void MutateFirstStructuredBufferMember(nlohmann::json &root)
    {
        for (nlohmann::json &parameter : root.at("parameters"))
        {
            if (!parameter.contains("type") || parameter.at("type").value("baseShape", "") != "structuredBuffer")
                continue;
            for (nlohmann::json &field : parameter.at("type").at("resultType").at("fields"))
            {
                nlohmann::json &binding = field.at("binding");
                if (binding.value("kind", "") == "uniform")
                {
                    binding["offset"] = binding.at("offset").get<uint32_t>() + 16;
                    return;
                }
            }
        }
        FAIL("the compiled reflection has no structured-buffer member");
    }

    void MutateFirstFloatVectorScalar(nlohmann::json &root)
    {
        for (nlohmann::json &parameter : root.at("parameters"))
        {
            if (!parameter.contains("type"))
                continue;
            for (nlohmann::json &field : parameter.at("type").at("elementType").at("fields"))
            {
                nlohmann::json &type = field.at("type");
                if (type.value("kind", "") == "vector" && type.at("elementType").value("scalarType", "") == "float32")
                {
                    type.at("elementType")["scalarType"] = "int32";
                    return;
                }
            }
        }
        FAIL("the compiled reflection has no float vector");
    }

    void MutateFragmentTarget(nlohmann::json &root)
    {
        nlohmann::json &result = root.at("entryPoints")[0].at("result");
        if (result.value("semanticName", "") == "SV_TARGET" && result.at("type").contains("elementCount"))
        {
            result.at("type")["elementCount"] =
                result.at("type").at("elementCount").get<uint32_t>() + 1;
            return;
        }
        for (nlohmann::json &field : result.at("type").at("fields"))
        {
            if (field.value("semanticName", "") != "SV_TARGET")
                continue;
            field.at("type")["elementCount"] = field.at("type").at("elementCount").get<uint32_t>() + 1;
            return;
        }
        FAIL("the compiled fragment reflection has no SV_TARGET");
    }
}

TEST_CASE("Actual Slang reflection rejects one ABI change per kind", "[renderer][shader][abi]")
{
    const auto manifest = LoadManifestJson();
    REQUIRE(manifest.has_value());

    const nlohmann::json *surfaceFragment = nullptr;
    for (const nlohmann::json &output : manifest->at("outputs"))
    {
        const std::string name = std::filesystem::path(output.at("output").get<std::string>()).filename().string();
        if (name.find("default_") != std::string::npos && output.at("entry") == "fragmentMain")
        {
            surfaceFragment = &output;
            break;
        }
    }
    REQUIRE(surfaceFragment != nullptr);
    const std::string surfaceText = ReflectionText(*surfaceFragment);
    std::string error;
    const auto surface = mir::ParseShaderReflection(surfaceText, "fragment", &error);
    INFO(error);
    REQUIRE(surface.has_value());
    REQUIRE_FALSE(surface->descriptors.empty());
    REQUIRE_FALSE(surface->pushConstants.empty());
    REQUIRE_FALSE(surface->outputs.empty());
    CHECK(surface->outputs[0].semantic.rfind("SV_TARGET", 0) == 0);

    nlohmann::json mutated = nlohmann::json::parse(surfaceText);
    MutateFirstPushSize(mutated);
    auto candidate = mir::ParseShaderReflection(mutated.dump(), "fragment", &error);
    REQUIRE(candidate.has_value());
    CHECK(mir::CompareShaderReflection(*surface, *candidate).restartRequired);

    mutated = nlohmann::json::parse(surfaceText);
    MutateFirstUniformOffset(mutated);
    candidate = mir::ParseShaderReflection(mutated.dump(), "fragment", &error);
    REQUIRE(candidate.has_value());
    CHECK(mir::CompareShaderReflection(*surface, *candidate).restartRequired);

    mutated = nlohmann::json::parse(surfaceText);
    MutateFirstFloatVectorScalar(mutated);
    candidate = mir::ParseShaderReflection(mutated.dump(), "fragment", &error);
    REQUIRE(candidate.has_value());
    CHECK(mir::CompareShaderReflection(*surface, *candidate).restartRequired);

    mutated = nlohmann::json::parse(surfaceText);
    MutateFragmentTarget(mutated);
    candidate = mir::ParseShaderReflection(mutated.dump(), "fragment", &error);
    REQUIRE(candidate.has_value());
    CHECK(mir::CompareShaderReflection(*surface, *candidate).restartRequired);

    const nlohmann::json *rayTrace = FindOutput(*manifest, "raytrace.closestHitMain.spv");
    REQUIRE(rayTrace != nullptr);
    const auto ray = mir::ParseShaderReflection(ReflectionText(*rayTrace), "closesthit", &error);
    INFO(error);
    REQUIRE(ray.has_value());
    REQUIRE_FALSE(ray->descriptors.empty());
    mutated = nlohmann::json::parse(ReflectionText(*rayTrace));
    MutateFirstStructuredBufferMember(mutated);
    candidate = mir::ParseShaderReflection(mutated.dump(), "closesthit", &error);
    REQUIRE(candidate.has_value());
    CHECK(mir::CompareShaderReflection(*ray, *candidate).restartRequired);
}

TEST_CASE("A deleted dependency becomes an invalid fingerprint and a new revision", "[renderer][shader][graph]")
{
    const std::filesystem::path directory = TestDirectory();
    const std::filesystem::path source = directory / "include.slang";
    {
        std::ofstream file(source);
        file << "// include\n";
    }
    mir::ShaderDependencyIndex index;
    index.AddOutput("out.spv");
    index.AddDependency(source.string(), "out.spv");
    REQUIRE(index.Poll().size() == 1);
    const uint64_t revision = index.Revision();
    REQUIRE(index.FingerprintOf(source.string())->valid);

    std::filesystem::remove(source);
    const std::vector<std::string> changed = index.Poll();
    REQUIRE(changed.size() == 1);
    CHECK(changed[0] == source.string());
    CHECK(index.Revision() == revision + 1);
    CHECK_FALSE(index.FingerprintOf(source.string())->valid);

    {
        std::ofstream file(source);
        file << "// restored\n";
    }
    const std::vector<std::string> restored = index.Poll();
    REQUIRE(restored.size() == 1);
    CHECK(index.FingerprintOf(source.string())->valid);
    CHECK(index.Revision() == revision + 2);

    std::filesystem::remove_all(directory);
}

TEST_CASE("Rollback after a successful file publication restores old bytes and stamps", "[renderer][shader][files]")
{
    const std::filesystem::path directory = TestDirectory();
    const std::filesystem::path existing = directory / "existing.spv";
    const std::filesystem::path added = directory / "added.spv";
    const std::string oldBytes = "old bytes";
    {
        std::ofstream file(existing, std::ios::binary);
        file << oldBytes;
    }
    {
        std::ofstream file((existing.string() + ".stamp").c_str());
        file << "9 900\n";
    }
    const std::vector<std::byte> newBytes = Words({ 0x07230203, 0x00010500, 0, 1, 0 });

    mir::StagedShaderPublisher publisher(directory, 10, 700);
    std::string error;
    REQUIRE(publisher.Stage("existing.spv", newBytes, &error));
    REQUIRE(publisher.Stage("added.spv", newBytes, &error));
    REQUIRE(publisher.Commit(&error));
    CHECK(mir::StagedShaderPublisher::ReadStamp(existing)->revision == 10);
    CHECK(std::filesystem::exists(added));

    publisher.Rollback();
    CHECK(ReadTestFile(existing) == oldBytes);
    const auto stamp = mir::StagedShaderPublisher::ReadStamp(existing);
    REQUIRE(stamp.has_value());
    CHECK(stamp->revision == 9);
    CHECK(stamp->sourceFingerprint == 900);
    CHECK_FALSE(std::filesystem::exists(added));
    CHECK_FALSE(std::filesystem::exists(added.string() + ".stamp"));

    std::filesystem::remove_all(directory);
}

TEST_CASE("Actual SPIR-V ABI rejects matrix major and scalar category changes", "[renderer][shader][spirv]")
{
    const std::filesystem::path directory = TestDirectory();
    const auto write = [&](const char *name, const char *body)
        {
            std::ofstream file(directory / name);
            file << body;
        };
    write("float.slang",
        "struct Data { float4x4 m; float4 v; };\n"
        "ConstantBuffer<Data> data;\n"
        "RWStructuredBuffer<float4> out;\n"
        "[numthreads(1, 1, 1)]\n"
        "void probeMain(uint3 id : SV_DispatchThreadID) { out[id.x] = mul(data.m, data.v); }\n");
    write("uint.slang",
        "struct Data { float4x4 m; uint4 v; };\n"
        "ConstantBuffer<Data> data;\n"
        "RWStructuredBuffer<float4> out;\n"
        "[numthreads(1, 1, 1)]\n"
        "void probeMain(uint3 id : SV_DispatchThreadID) { out[id.x] = data.v; }\n");
    write("body.slang",
        "struct Data { float4x4 m; float4 v; };\n"
        "ConstantBuffer<Data> data;\n"
        "RWStructuredBuffer<float4> out;\n"
        "[numthreads(1, 1, 1)]\n"
        "void probeMain(uint3 id : SV_DispatchThreadID) { out[id.x] = data.v + data.v; }\n");

    const auto compile = [&](const char *source, const char *matrix, const char *output)
        {
            const std::string command = "\"" + std::string(MIR_SLANGC_EXECUTABLE) + "\" \"" +
                (directory / source).string() + "\" -entry probeMain -stage compute -target spirv " +
                "-matrix-layout-column-major " + matrix + " -fvk-use-gl-layout " +
                "-fvk-use-entrypoint-name -o \"" + (directory / output).string() + "\"";
            int result = -1;
            for (int attempt = 0; attempt < 3; ++attempt)
            {
                result = RunCommand(command, MIR_SLANGC_EXECUTABLE);
                if (result == 0)
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
            INFO(command);
            INFO(result);
            return result;
        };
    REQUIRE(compile("float.slang", "-matrix-layout-column-major", "column.spv") == 0);
    REQUIRE(compile("float.slang", "-matrix-layout-row-major", "row.spv") == 0);
    REQUIRE(compile("body.slang", "-matrix-layout-column-major", "body.spv") == 0);
    REQUIRE(compile("uint.slang", "-matrix-layout-column-major", "uint.spv") == 0);

    const auto column = mir::ParseSpirvAbi(*mir::ReadBytes(directory / "column.spv"), "probeMain");
    const auto row = mir::ParseSpirvAbi(*mir::ReadBytes(directory / "row.spv"), "probeMain");
    const auto body = mir::ParseSpirvAbi(*mir::ReadBytes(directory / "body.spv"), "probeMain");
    const auto uintValue = mir::ParseSpirvAbi(*mir::ReadBytes(directory / "uint.spv"), "probeMain");
    REQUIRE(column.has_value());
    REQUIRE(row.has_value());
    REQUIRE(body.has_value());
    REQUIRE(uintValue.has_value());
    CHECK(mir::CompareSpirvAbi(*column, *row).restartRequired);
    CHECK(mir::CompareSpirvAbi(*column, *body).compatible);
    INFO(column->descriptors[0]);
    INFO(uintValue->descriptors[0]);
    CHECK(mir::CompareSpirvAbi(*column, *uintValue).restartRequired);
    std::filesystem::remove_all(directory);
}
TEST_CASE("Shader generations publish immutable content snapshots with a parent", "[renderer][shader][generation]")
{
    const std::filesystem::path root = TestDirectory() / "shaders";
    std::filesystem::create_directories(root);
    const std::filesystem::path input = root / "input.slang";
    std::filesystem::create_directories(root / "generations");
    {
        std::ofstream file(input);
        file << "first\n";
    }
    const auto inputSnapshot = mir::SnapshotShaderFile(input, input.string());
    REQUIRE(inputSnapshot.has_value());

    mir::ShaderGenerationStore first(root);
    uint64_t firstId = 0;
    std::string error;
    REQUIRE(first.BeginPublish(&firstId, &error));
    const std::vector<std::byte> bytes{ std::byte{ 1 }, std::byte{ 2 } };
    REQUIRE(first.StageFile("out.spv", bytes, &error));
    first.SetInputs({ *inputSnapshot });
    REQUIRE(first.Finish(&error));
    REQUIRE(first.PublishPointer(&error));
    first.Unlock();

    const auto current = mir::ShaderGenerationStore::ReadCurrent(root, &error);
    REQUIRE(current.has_value());
    CHECK(current->mId == firstId);
    CHECK(mir::ShaderGenerationStore::ReadGeneration(root, firstId, &error).has_value());

    {
        std::ofstream file(input);
        file << "changed\n";
    }
    mir::ShaderGenerationStore stale(root);
    uint64_t staleId = 0;
    REQUIRE(stale.BeginPublish(&staleId, &error));
    REQUIRE(stale.StageFile("out.spv", bytes, &error));
    stale.SetInputs({ *inputSnapshot });
    CHECK_FALSE(stale.Finish(&error));
    stale.Abort();
    stale.Unlock();
    CHECK(mir::ShaderGenerationStore::ReadCurrent(root, &error)->mId == firstId);
    std::filesystem::remove_all(root.parent_path());
}
TEST_CASE("Pointer promotion rechecks the input snapshot", "[renderer][shader][generation]")
{
    const std::filesystem::path root = TestDirectory() / "shaders";
    std::filesystem::create_directories(root);
    const std::filesystem::path input = root / "input.slang";
    {
        std::ofstream file(input);
        file << "before\n";
    }
    const auto snapshot = mir::SnapshotShaderFile(input, input.string());
    REQUIRE(snapshot.has_value());
    mir::ShaderGenerationStore store(root);
    uint64_t id = 0;
    std::string error;
    REQUIRE(store.BeginPublish(&id, &error));
    const std::vector<std::byte> bytes{ std::byte{ 3 } };
    REQUIRE(store.StageFile("out.spv", bytes, &error));
    store.SetInputs({ *snapshot });
    REQUIRE(store.Finish(&error));
    {
        std::ofstream file(input);
        file << "after\n";
    }
    CHECK_FALSE(store.PublishPointer(&error));
    CHECK_FALSE(mir::ShaderGenerationStore::ReadCurrent(root, &error).has_value());
    store.Unlock();
    std::error_code ignored;
    std::filesystem::remove_all(root.parent_path(), ignored);
}

TEST_CASE("Sealed unpublished generations do not block later publication", "[renderer][shader][generation]")
{
    const auto root = TestDirectory() / "shaders";
    uint64_t abandonedId = 0;
    std::string error;
    const std::vector<std::byte> bytes{std::byte{1}};
    {
        mir::ShaderGenerationStore abandoned(root);
        REQUIRE(abandoned.BeginPublish(&abandonedId, &error));
        REQUIRE(abandoned.StageFile("probe.spv", bytes, &error));
        REQUIRE(abandoned.Finish(&error));
    }
    mir::ShaderGenerationStore next(root);
    uint64_t nextId = 0;
    REQUIRE(next.BeginPublish(&nextId, &error));
    CHECK(nextId > abandonedId);
    REQUIRE(next.StageFile("probe.spv", bytes, &error));
    REQUIRE(next.Finish(&error));
    REQUIRE(next.PublishPointer(&error));
    next.Unlock();
    CHECK(mir::ShaderGenerationStore::ReadGeneration(root, abandonedId, &error));
    CHECK(mir::ShaderGenerationStore::ReadCurrent(root, &error)->mId == nextId);
    std::filesystem::remove_all(root.parent_path());
}

TEST_CASE("Malformed generation metadata is rejected before returning a directory", "[renderer][shader][generation]")
{
    const auto root = TestDirectory() / "shaders";
    const auto generation = root / "generations" / "0000000000000001";
    std::filesystem::create_directories(generation);
    const std::vector<std::string> metadata{
        "{}", R"({"version":1,"generation":1,"parent":0})",
        R"({"version":1,"generation":1,"parent":"bad","outputs":[],"inputs":[]})",
        R"({"version":1,"generation":2,"parent":0,"outputs":[],"inputs":[]})",
        R"({"version":1,"generation":1,"parent":0,"outputs":[{}],"inputs":[]})"};
    for (const auto &text : metadata)
    {
        { std::ofstream file(generation / "metadata.json"); file << text; }
        std::string error;
        CHECK_FALSE(mir::ShaderGenerationStore::ReadGeneration(root, 1, &error));
        CHECK_FALSE(error.empty());
    }
    std::filesystem::remove_all(root.parent_path());
}
