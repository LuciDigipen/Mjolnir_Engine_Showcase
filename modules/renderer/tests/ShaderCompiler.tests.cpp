/**
 * @file ShaderCompiler.tests.cpp
 * @author Jackey Cen
 * @brief Tests for renderer/ShaderCompiler.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 */
#include <core/fs/FileIO.h>
#include <renderer/ShaderCompiler.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <thread>

namespace
{
    std::string Utf8(const std::filesystem::path &path)
    {
        const auto bytes = path.generic_u8string();
        return {reinterpret_cast<const char *>(bytes.data()), bytes.size()};
    }

    struct Fixture
    {
        std::filesystem::path root = mir::UniqueShaderJobDirectory(
            std::filesystem::u8path(MIR_RENDERER_TEST_DIR) / std::filesystem::u8path("compiler space-\xe6\xb5\x8b\xe8\xaf\x95"));
        ~Fixture() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
    };
}

TEST_CASE("Compiler cache checks include content when timestamps are preserved", "[renderer][shader][compiler]")
{
    Fixture fixture;
    const auto source = fixture.root / "probe.slang";
    const auto include = fixture.root / "value.slang";
    REQUIRE(mir::WriteText(source, "#include \"value.slang\"\nRWStructuredBuffer<float> result;\n"
        "[shader(\"compute\")][numthreads(1,1,1)] void probeMain() { result[0] = Value(); }\n"));
    REQUIRE(mir::WriteText(include, "float Value() { return 1.0; }\n"));
    mir::ShaderManifest manifest;
    manifest.slangc = std::filesystem::u8path(MIR_SLANGC_EXECUTABLE);
    manifest.workingDirectory = fixture.root;
    mir::ShaderManifestOutput output;
    output.output = Utf8(fixture.root / "probe.spv");
    output.reflection = output.output + ".reflection.json";
    output.depfile = output.output + ".d";
    output.entry = "probeMain";
    output.stage = "compute";
    output.sources = {Utf8(source)};
    output.arguments = {Utf8(manifest.slangc), Utf8(source), "-target", "spirv", "-profile", "spirv_1_5",
        "-fvk-use-entrypoint-name", "-entry", output.entry, "-o", output.output,
        "-reflection-json", output.reflection, "-depfile", output.depfile};
    const auto cache = fixture.root / "cache";
    const auto first = mir::CompileShaderArtifact(manifest, output, fixture.root / "first", cache);
    INFO(first.diagnostic);
    REQUIRE(first.success);
    CHECK_FALSE(first.cached);
    REQUIRE(std::any_of(first.inputs.begin(), first.inputs.end(), [&](const auto &input)
        { return std::filesystem::u8path(input.mPath) == include; }));
    const auto firstBytes = mir::ReadBytes(std::filesystem::u8path(first.output.output));
    REQUIRE(firstBytes);
    const auto second = mir::CompileShaderArtifact(manifest, output, fixture.root / "second", cache);
    INFO(second.diagnostic);
    REQUIRE(second.success);
    CHECK(second.cached);
    const auto time = std::filesystem::last_write_time(include);
    REQUIRE(mir::WriteText(include, "float Value() { return 2.0; }\n"));
    std::filesystem::last_write_time(include, time);
    const auto third = mir::CompileShaderArtifact(manifest, output, fixture.root / "third", cache);
    INFO(third.diagnostic);
    REQUIRE(third.success);
    CHECK_FALSE(third.cached);
    CHECK(mir::ReadBytes(std::filesystem::u8path(third.output.output)) != firstBytes);
    REQUIRE(mir::WriteText(include, "broken source\n"));
    const auto failed = mir::CompileShaderArtifact(manifest, output, fixture.root / "fourth", cache);
    CHECK_FALSE(failed.success);
    CHECK(mir::ReadBytes(std::filesystem::u8path(first.output.output)) == firstBytes);
}

#if defined(_WIN32)
TEST_CASE("Compiler jobs drain excess output and support bounded cancellation", "[renderer][shader][compiler]")
{
    Fixture fixture;
    const std::filesystem::path powershell = "C:/Windows/System32/WindowsPowerShell/v1.0/powershell.exe";
    const auto script = fixture.root / "pressure.ps1";
    REQUIRE(mir::WriteText(script, "[Console]::Out.Write(('x' * 200000)); exit 7\n"));
    const auto started = std::chrono::steady_clock::now();
    const std::vector<std::string> args{"-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", Utf8(script)};
    const auto pressure = mir::RunShaderCompiler(powershell, args, fixture.root, nullptr, std::chrono::seconds(10));
    CHECK_FALSE(pressure.success);
    CHECK(pressure.exitCode == 7);
    CHECK(pressure.diagnostic.size() == 64 * 1024);
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(10));
    REQUIRE(mir::WriteText(script, "Start-Sleep -Seconds 20\n"));
    std::atomic_bool cancel{false};
    std::jthread cancellation([&] { std::this_thread::sleep_for(std::chrono::milliseconds(150)); cancel = true; });
    const auto cancelled = mir::RunShaderCompiler(powershell, args, fixture.root, &cancel);
    CHECK(cancelled.cancelled);
    CHECK_FALSE(cancelled.success);
    const auto timeout = mir::RunShaderCompiler(powershell, args, fixture.root, nullptr, std::chrono::milliseconds(150));
    CHECK_FALSE(timeout.success);
    CHECK(timeout.diagnostic.find("timed out") != std::string::npos);
    const auto missing = mir::RunShaderCompiler(fixture.root / "missing.exe", {}, fixture.root);
    CHECK_FALSE(missing.success);
}
#endif
