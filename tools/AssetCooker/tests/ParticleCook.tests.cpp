/**
 * @file ParticleCook.tests.cpp
 * @author sumin.park
 * @brief Cooks the preset through AssetCooker and validates runtime dependencies.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <assets/AssetCache.h>
#include <assets/AssetManifest.h>
#include <core/fs/FileIO.h>
#include <core/platform/Process.h>
#include <particles/ParticleEffect.h>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace
{
    struct CookFixture
    {
        std::filesystem::path parent = std::filesystem::canonical(std::filesystem::current_path());
        std::filesystem::path root = parent / ("particle-cook-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::path source = root / "source";
        std::filesystem::path out = root / "cooked";

        CookFixture()
        {
            if (!std::filesystem::create_directory(root))
                throw std::runtime_error("cannot create particle cook fixture");
            std::filesystem::create_directory(source);
            std::filesystem::copy_file(std::filesystem::path(GLTF_FIXTURE_DIR) / "checker.png",
                source / "checker.png");
            std::filesystem::copy_file(std::filesystem::path(PARTICLE_FIXTURE_DIR) / "exhaust.particle.json",
                source / "exhaust.particle.json");
            std::filesystem::copy_file(std::filesystem::path(PARTICLE_FIXTURE_DIR) / "exhaust.emitter.json",
                source / "exhaust.emitter.json");
        }
        ~CookFixture()
        {
            std::error_code error;
            if (std::filesystem::weakly_canonical(root, error).parent_path() == parent && !error)
                std::filesystem::remove_all(root, error);
        }
        int Cook(const std::filesystem::path &changed = {}, const std::filesystem::path &result = {}) const
        {
            std::vector<std::string> args{
                "--source-dir", source.string(), "--id-root", "assets", "--out", out.string()};
            if (!result.empty())
            {
                args.push_back("--result");
                args.push_back(result.string());
            }
            if (!changed.empty())
            {
                args.push_back("--changed");
                args.push_back(changed.string());
            }
            return mir::LaunchProcess(GLTF_COOKER_EXE, args, true).value_or(-1);
        }
    };
}

TEST_CASE("Particle system cooks and resolves its emitter and typed texture dependencies", "[particles][cooker]")
{
    CookFixture fixture;
    REQUIRE(fixture.Cook() == 0);
    auto manifest = mir::AssetManifest::LoadFile(fixture.out / mir::kAssetManifestFileName);
    REQUIRE(manifest);
    CHECK(manifest->Count() == 3);
    const auto id = mir::MakeAssetId("assets/exhaust.particle.json");
    const auto *entry = manifest->Find(id);
    REQUIRE(entry);
    CHECK(entry->typeTag == mir::kParticleEffectTypeTag);
    CHECK(entry->contentVersion == mir::kParticleEffectContentVersion);
    mir::AssetCache cache(&*manifest, fixture.out);
    const auto loaded = mir::LoadParticleEffect(cache, id);
    REQUIRE(loaded.mEffect);
    REQUIRE(loaded.mEffect->emitters.size() == 1);
    CHECK(loaded.mEffect->emitters[0] == mir::MakeAssetId("assets/exhaust.emitter.json"));
    REQUIRE(loaded.mEffect->emitterDefinitions.size() == 1);
    CHECK(loaded.mEffect->emitterDefinitions[0].texture == mir::MakeAssetId("assets/checker.png"));
    CHECK(loaded.mEffect->emitterDefinitions[0].spawnRate == 60.0f);
    const auto *emitterEntry = manifest->Find(loaded.mEffect->emitters[0]);
    REQUIRE(emitterEntry);
    CHECK(emitterEntry->typeTag == mir::kParticleEmitterTypeTag);
    CHECK(emitterEntry->contentVersion == mir::kParticleEmitterContentVersion);

    const auto oldManifest = mir::ReadBytes(fixture.out / mir::kAssetManifestFileName);
    std::filesystem::remove(fixture.source / "checker.png");
    CHECK(fixture.Cook() != 0);
    CHECK(mir::ReadBytes(fixture.out / mir::kAssetManifestFileName) == oldManifest);
    CHECK(fixture.Cook(fixture.source / "checker.png") != 0);
    CHECK(mir::ReadBytes(fixture.out / mir::kAssetManifestFileName) == oldManifest);
}

TEST_CASE("Particle cooker rejects unsupported source versions before publishing a manifest", "[particles][cooker]")
{
    CookFixture fixture;
    std::ofstream(fixture.source / "exhaust.particle.json") <<
        R"({"version":999,"emitters":["exhaust.emitter.json"]})";
    CHECK(fixture.Cook() != 0);
    CHECK_FALSE(std::filesystem::exists(fixture.out / mir::kAssetManifestFileName));
}

TEST_CASE("Particle cooker preserves the manifest when an emitter dependency disappears", "[particles][cooker]")
{
    CookFixture fixture;
    REQUIRE(fixture.Cook() == 0);
    const auto oldManifest = mir::ReadBytes(fixture.out / mir::kAssetManifestFileName);
    std::filesystem::remove(fixture.source / "exhaust.emitter.json");
    CHECK(fixture.Cook(fixture.source / "exhaust.emitter.json") != 0);
    CHECK(mir::ReadBytes(fixture.out / mir::kAssetManifestFileName) == oldManifest);
}

TEST_CASE("Particle emitter changes recook the dependent system graph", "[particles][cooker]")
{
    CookFixture fixture;
    const auto resultPath = fixture.root / "result.json";
    REQUIRE(fixture.Cook({}, resultPath) == 0);
    auto readResult = [&]
    {
        std::ifstream stream(resultPath);
        return nlohmann::json::parse(stream);
    };
    auto result = readResult();
    CHECK(result.value("assetsChanged", false));
    const auto oldManifest = mir::ReadBytes(fixture.out / mir::kAssetManifestFileName);
    std::ofstream(fixture.source / "exhaust.emitter.json") <<
        R"({"version":1,"target":"gpu","lifetime":2,"spawnRate":120,"capacity":256,"texture":"checker.png",
            "renderer":{"size":0.2,"color":[1,0,0,0.25],"velocity":[0,2,0]}})";
    REQUIRE(fixture.Cook(fixture.source / "exhaust.emitter.json", resultPath) == 0);
    result = readResult();
    CHECK(result.value("assetsChanged", false));
    CHECK(mir::ReadBytes(fixture.out / mir::kAssetManifestFileName) != oldManifest);
    auto manifest = mir::AssetManifest::LoadFile(fixture.out / mir::kAssetManifestFileName);
    REQUIRE(manifest);
    mir::AssetCache cache(&*manifest, fixture.out);
    const auto loaded = mir::LoadParticleEffect(cache, mir::MakeAssetId("assets/exhaust.particle.json"));
    REQUIRE(loaded.mEffect);
    REQUIRE(loaded.mEffect->emitterDefinitions.size() == 1);
    const auto &emitter = loaded.mEffect->emitterDefinitions[0];
    CHECK(emitter.spawnRate == 120);
    CHECK(emitter.size == 0.2f);
    CHECK(emitter.color.a == 0.25f);
    CHECK(emitter.velocity.y == 2);
    REQUIRE(fixture.Cook(fixture.source / "exhaust.emitter.json", resultPath) == 0);
    result = readResult();
    CHECK_FALSE(result.value("assetsChanged", true));
}

TEST_CASE("Particle-only recook retains assets from other source roots", "[particles][cooker]")
{
    CookFixture fixture;
    const auto secondRoot = fixture.root / "other-source";
    std::filesystem::create_directory(secondRoot);
    std::ofstream(secondRoot / "retained.txt") << "other game asset";
    const auto args = [&](bool changed)
    {
        std::vector<std::string> values{"--source-dir", fixture.source.string(), "--id-root", "assets",
                                        "--source-dir", secondRoot.string(), "--id-root", "other",
                                        "--out", fixture.out.string()};
        if (changed)
        {
            values.push_back("--changed");
            values.push_back((fixture.source / "exhaust.emitter.json").string());
        }
        return mir::LaunchProcess(GLTF_COOKER_EXE, values, true).value_or(-1);
    };
    REQUIRE(args(false) == 0);
    const auto retained = mir::MakeAssetId("other/retained.txt");
    auto manifest = mir::AssetManifest::LoadFile(fixture.out / mir::kAssetManifestFileName);
    REQUIRE(manifest);
    REQUIRE(manifest->Find(retained));

    std::ofstream(fixture.source / "exhaust.emitter.json") <<
        R"({"version":1,"target":"gpu","lifetime":2,"spawnRate":120,"capacity":256,"texture":"checker.png"})";
    REQUIRE(args(true) == 0);
    manifest = mir::AssetManifest::LoadFile(fixture.out / mir::kAssetManifestFileName);
    REQUIRE(manifest);
    CHECK(manifest->Find(retained));
}
