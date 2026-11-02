/**
 * @file ShaderReload.tests.cpp
 * @author Jackey Cen
 * @brief Tests for renderer/ShaderReload.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer/ShaderReload.h>

#include <catch2/catch_test_macros.hpp>

#include <stdexcept>
#include <string>
#include <vector>

TEST_CASE("A failed shader reload stage keeps the previous whole generation", "[renderer][shader][transaction]")
{
    mir::ShaderReloadTransaction transaction;
    std::vector<std::string> journal;

    transaction.AddStage("materials",
        [&](std::string &)
        {
            journal.push_back("prepare materials");
            return true;
        },
        [&] { journal.push_back("publish materials"); },
        [&] { journal.push_back("discard materials"); });
    transaction.AddStage("raytracing",
        [&](std::string &error)
        {
            journal.push_back("prepare raytracing");
            error = "candidate pipeline failed";
            return false;
        },
        [&] { journal.push_back("publish raytracing"); },
        [&] { journal.push_back("discard raytracing"); });
    transaction.AddStage("post",
        [&](std::string &)
        {
            journal.push_back("prepare post");
            return true;
        },
        [&] { journal.push_back("publish post"); },
        [&] { journal.push_back("discard post"); });

    std::string error;
    CHECK_FALSE(transaction.PrepareAll(&error));
    CHECK(error.find("raytracing") != std::string::npos);
    CHECK(error.find("candidate pipeline failed") != std::string::npos);
    CHECK(journal == std::vector<std::string>{ "prepare materials", "prepare raytracing",
                  "discard raytracing", "discard materials" });
    CHECK_FALSE(transaction.IsPrepared());
}

TEST_CASE("Every shader reload stage publishes only after all prepares succeed", "[renderer][shader][transaction]")
{
    mir::ShaderReloadTransaction transaction;
    std::vector<std::string> journal;

    for (const char *name : { "materials", "raytracing", "shadows", "post" })
    {
        const std::string stageName(name);
        transaction.AddStage(stageName,
            [&, stageName](std::string &)
            {
                journal.push_back("prepare " + stageName);
                return true;
            },
            [&, stageName] { journal.push_back("publish " + stageName); },
            [&, stageName] { journal.push_back("discard " + stageName); });
    }

    std::string error;
    REQUIRE(transaction.PrepareAll(&error));
    CHECK(transaction.IsPrepared());
    CHECK(transaction.StageNames().size() == 4);
    CHECK(journal == std::vector<std::string>{ "prepare materials", "prepare raytracing",
                  "prepare shadows", "prepare post" });

    transaction.CommitAll();
    CHECK(journal == std::vector<std::string>{ "prepare materials", "prepare raytracing",
                  "prepare shadows", "prepare post", "publish materials", "publish raytracing",
                  "publish shadows", "publish post" });
    CHECK(transaction.StageCount() == 0);
}

TEST_CASE("A stale revision cannot publish", "[renderer][shader][transaction]")
{
    mir::ShaderDependencyIndex index;
    index.AddDependency("material.slang", "material.fragmentMain.spv");
    index.Observe("material.slang", mir::ShaderSourceFingerprint{ 1, 10, true });
    const uint64_t batchRevision = index.Revision();
    index.Observe("material.slang", mir::ShaderSourceFingerprint{ 2, 20, true });

    bool published = false;
    mir::ShaderReloadTransaction transaction;
    transaction.AddStage("materials",
        [&](std::string &error)
        {
            if (!index.IsCurrent(batchRevision))
            {
                error = "superseded by a newer source revision";
                return false;
            }
            return true;
        },
        [&] { published = true; },
        [] {});

    std::string error;
    CHECK_FALSE(transaction.PrepareAll(&error));
    CHECK_FALSE(published);
    CHECK(error.find("superseded") != std::string::npos);
}

TEST_CASE("Coverage validation names every live shader call site", "[renderer][shader][library]")
{
    mir::ShaderManifest manifest;
    mir::ShaderManifestOutput legacy;
    legacy.output = "model.vertexMain.spv";
    legacy.stage = "vertex";
    mir::ShaderManifestOutput surface;
    surface.output = "default_DefaultMaterial_1.fragmentMain.spv";
    surface.stage = "fragment";
    manifest.outputs.push_back(legacy);
    manifest.outputs.push_back(surface);

    mir::ShaderLibrary library;
    library.Register("MaterialStore", { legacy.output, surface.output }, { "vertex", "fragment" });
    std::string error;
    REQUIRE(library.ValidateCoverage(manifest, &error));

    mir::ShaderManifestOutput shadow;
    shadow.output = "shadowDepth.vertexMain.spv";
    shadow.stage = "vertex";
    manifest.outputs.push_back(shadow);
    library.Register("ShadowMaps", { shadow.output }, { "vertex" });
    REQUIRE(library.ValidateCoverage(manifest, &error));

    mir::ShaderLibrary incomplete;
    incomplete.Register("Bloom", { "bloom.upsampleMain.spv" }, { "vertex" });
    error.clear();
    CHECK_FALSE(incomplete.ValidateCoverage(manifest, &error));
    CHECK(error.find("Bloom") != std::string::npos);

    mir::ShaderLibrary wrongStage;
    wrongStage.Register("Velocity", { legacy.output }, { "fragment" });
    error.clear();
    CHECK_FALSE(wrongStage.ValidateCoverage(manifest, &error));
    CHECK(error.find("stage") != std::string::npos);
}

TEST_CASE("A throwing prepare discards its own and earlier candidates", "[renderer][shader][transaction]")
{
    mir::ShaderReloadTransaction transaction;
    std::vector<std::string> journal;
    transaction.AddStage("first",
        [&](std::string &)
        {
            journal.push_back("prepare first");
            return true;
        },
        [&] { journal.push_back("publish first"); },
        [&] { journal.push_back("discard first"); });
    transaction.AddStage("second",
        [&](std::string &) -> bool
        {
            journal.push_back("prepare second");
            throw std::runtime_error("second threw");
        },
        [&] { journal.push_back("publish second"); },
        [&] { journal.push_back("discard second"); });

    std::string error;
    CHECK_FALSE(transaction.PrepareAll(&error));
    CHECK(error.find("second") != std::string::npos);
    CHECK(error.find("second threw") != std::string::npos);
    CHECK(journal == std::vector<std::string>{ "prepare first", "prepare second", "discard second",
                  "discard first" });
}

TEST_CASE("Shader outputs without reload owners must remain unchanged", "[renderer][shader][transaction]")
{
    const std::map<std::string, uint64_t> accepted{{"material.spv", 1}, {"ssgi.spv", 2}};
    CHECK(mir::ValidateShaderOwnerCoverage({"material.spv"}, accepted,
        {{"material.spv", 3}, {"ssgi.spv", 2}}).compatible);
    CHECK_FALSE(mir::ValidateShaderOwnerCoverage({"material.spv"}, accepted,
        {{"material.spv", 3}, {"ssgi.spv", 4}}).compatible);
    CHECK_FALSE(mir::ValidateShaderOwnerCoverage({"material.spv"}, accepted,
        {{"material.spv", 1}}).compatible);
    CHECK_FALSE(mir::ValidateShaderOwnerCoverage({"material.spv"}, accepted,
        {{"material.spv", 1}, {"ssgi.spv", 2}, {"particles.spv", 3}}).compatible);
    CHECK(mir::ValidateShaderOwnerCoverage({"material.spv", "ssgi.spv"}, accepted,
        {{"material.spv", 3}, {"ssgi.spv", 4}}).compatible);
}
