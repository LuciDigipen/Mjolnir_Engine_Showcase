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
    CHECK(manifest->Count() == 5);
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

TEST_CASE("Particle stacks share scripts and changed stacks replace dependencies", "[particles][cooker]")
{
    CookFixture fixture;
    std::filesystem::copy_file(fixture.source / "exhaust.emitter.json", fixture.source / "second.emitter.json");
    REQUIRE(fixture.Cook() == 0);
    auto manifest = mir::AssetManifest::LoadFile(fixture.out / mir::kAssetManifestFileName);
    REQUIRE(manifest);
    CHECK(manifest->Count() == 6);
    mir::AssetCache cache(&*manifest, fixture.out);
    const auto first = mir::LoadParticleEmitterAsset(cache, mir::MakeAssetId("assets/exhaust.emitter.json"));
    const auto second = mir::LoadParticleEmitterAsset(cache, mir::MakeAssetId("assets/second.emitter.json"));
    REQUIRE(first.mEmitter);
    REQUIRE(second.mEmitter);
    CHECK(first.mEmitter->script == second.mEmitter->script);
    CHECK(first.mEmitter->scriptLayout == second.mEmitter->scriptLayout);
    CHECK_FALSE(first.mEmitter->scriptCode.empty());

    std::filesystem::copy_file(std::filesystem::path(PARTICLE_FIXTURE_DIR) / "accelerated.emitter.json",
        fixture.source / "second.emitter.json", std::filesystem::copy_options::overwrite_existing);
    REQUIRE(fixture.Cook(fixture.source / "second.emitter.json") == 0);
    auto changedManifest = mir::AssetManifest::LoadFile(fixture.out / mir::kAssetManifestFileName);
    REQUIRE(changedManifest);
    CHECK(changedManifest->Count() == 7);
    mir::AssetCache changedCache(&*changedManifest, fixture.out);
    const auto changed = mir::LoadParticleEmitterAsset(changedCache, mir::MakeAssetId("assets/second.emitter.json"));
    REQUIRE(changed.mEmitter);
    CHECK(changed.mEmitter->script != first.mEmitter->script);
    CHECK(changed.mEmitter->scriptCode != first.mEmitter->scriptCode);
    CHECK(changed.mEmitter->scriptLayout == first.mEmitter->scriptLayout);

    std::filesystem::copy_file(fixture.source / "exhaust.emitter.json", fixture.source / "second.emitter.json",
        std::filesystem::copy_options::overwrite_existing);
    REQUIRE(fixture.Cook(fixture.source / "second.emitter.json") == 0);
    auto reverted = mir::AssetManifest::LoadFile(fixture.out / mir::kAssetManifestFileName);
    REQUIRE(reverted);
    CHECK(reverted->Count() == 6);
}

TEST_CASE("Invalid particle module stacks retain the previous manifest", "[particles][cooker]")
{
    CookFixture fixture;
    REQUIRE(fixture.Cook() == 0);
    const auto previous = mir::ReadBytes(fixture.out / mir::kAssetManifestFileName);
    std::ifstream input(fixture.source / "exhaust.emitter.json");
    const auto source = nlohmann::json::parse(input);
    input.close();
    for (const auto &modules : {
        nlohmann::json::parse(R"([{"name":"Unknown"}])"),
        nlohmann::json::parse(R"([{"name":"Initialize"}])"),
        nlohmann::json::parse(R"([{"name":"Acceleration","parameters":[1,2]}])")})
    {
        auto changed = source;
        changed["updateModules"] = modules;
        std::ofstream(fixture.source / "exhaust.emitter.json") << changed.dump();
        CHECK(fixture.Cook(fixture.source / "exhaust.emitter.json") != 0);
        CHECK(mir::ReadBytes(fixture.out / mir::kAssetManifestFileName) == previous);
    }
    auto changed = source;
    changed["target"] = "cpu";
    std::ofstream(fixture.source / "exhaust.emitter.json") << changed.dump();
    CHECK(fixture.Cook() != 0);
    CHECK(mir::ReadBytes(fixture.out / mir::kAssetManifestFileName) == previous);
}

TEST_CASE("Particle cooker compiles moving exhaust modules and shares scripts across motion inputs", "[particles][cooker]")
{
    CookFixture fixture;
    const auto emitter = nlohmann::json::parse(R"({"version":1,"target":"gpu","lifetime":3,"spawnRate":90,"capacity":512,
        "texture":"checker.png","localOffset":[-1,0,0],"spawnPerMeter":12,"burst":8,"seed":99,
        "inheritVelocity":0.8,"teleportDistance":150,
        "spawnModules":[{"name":"Initialize"},{"name":"VelocityVariation","parameters":[0.2,0.3,0.4]}],
        "updateModules":[{"name":"Age"},{"name":"Drag","parameters":[0.4]},{"name":"Integrate"},
            {"name":"SizeOverLife","parameters":[2]},{"name":"FadeOverLife","parameters":[0]},
            {"name":"Flipbook","parameters":[2,2,12]}]})");
    std::ofstream(fixture.source / "exhaust.emitter.json") << emitter.dump();
    auto second = emitter;
    second["localOffset"] = {1, 0, 0};
    second["spawnPerMeter"] = 20;
    second["burst"] = 4;
    second["seed"] = 1234;
    second["inheritVelocity"] = 0.5;
    second["teleportDistance"] = 80;
    std::ofstream(fixture.source / "second.emitter.json") << second.dump();
    std::ofstream(fixture.source / "exhaust.particle.json") <<
        R"({"version":2,"emitters":["exhaust.emitter.json","second.emitter.json"],"parameters":{"wind":[8,0,-2]}})";
    REQUIRE(fixture.Cook() == 0);
    const auto manifest = mir::AssetManifest::LoadFile(fixture.out / mir::kAssetManifestFileName);
    REQUIRE(manifest);
    CHECK(manifest->Count() == 6);
    mir::AssetCache cache(&*manifest, fixture.out);
    const auto system = mir::LoadParticleEffect(cache, mir::MakeAssetId("assets/exhaust.particle.json"));
    REQUIRE(system.mEffect);
    REQUIRE(system.mEffect->emitterDefinitions.size() == 2);
    CHECK(system.mEffect->parameters.wind == glm::vec3(8, 0, -2));
    const auto &firstLoaded = system.mEffect->emitterDefinitions[0];
    const auto &secondLoaded = system.mEffect->emitterDefinitions[1];
    CHECK(firstLoaded.script == secondLoaded.script);
    CHECK(firstLoaded.scriptCode == secondLoaded.scriptCode);
    CHECK(firstLoaded.localOffset == glm::vec3(-1, 0, 0));
    CHECK(secondLoaded.localOffset == glm::vec3(1, 0, 0));
    CHECK(firstLoaded.spawnPerMeter == 12);
    CHECK(firstLoaded.burst == 8);
    CHECK(firstLoaded.seed == 99);
    CHECK(firstLoaded.inheritVelocity == 0.8f);
    CHECK(firstLoaded.teleportDistance == 150);
    CHECK(firstLoaded.textureWidth > 0);
    CHECK(firstLoaded.textureHeight > 0);
    CHECK(firstLoaded.texturePixels.size() == size_t(firstLoaded.textureWidth) * firstLoaded.textureHeight * 4);
    const auto *script = manifest->Find(firstLoaded.script);
    const auto *layout = manifest->Find(firstLoaded.scriptLayout);
    REQUIRE(script);
    REQUIRE(layout);
    CHECK(script->typeTag == mir::kParticleScriptTypeTag);
    CHECK(script->contentVersion == mir::kParticleScriptContentVersion);
    CHECK(layout->typeTag == mir::kParticleScriptLayoutTypeTag);
    CHECK(layout->contentVersion == mir::kParticleScriptLayoutContentVersion);
}

TEST_CASE("CPU ribbon stacks cook and load without GPU script dependencies", "[particles][cooker][ribbon]")
{
    CookFixture fixture;
    std::ofstream(fixture.source / "exhaust.emitter.json") << R"({
        "version":1,"target":"cpu","lifetime":30,"spawnRate":0,"spawnPerMeter":0.5,
        "capacity":512,"texture":"checker.png",
        "spawnModules":[{"name":"RibbonSpawn","parameters":[0.25]}],
        "updateModules":[{"name":"RibbonAge"},{"name":"RibbonWind"},
            {"name":"RibbonWidth","parameters":[8]},{"name":"RibbonFade"}]})";
    REQUIRE(fixture.Cook() == 0);
    auto manifest = mir::AssetManifest::LoadFile(fixture.out / mir::kAssetManifestFileName);
    REQUIRE(manifest);
    CHECK(manifest->Count() == 3);
    mir::AssetCache cache(&*manifest,fixture.out);
    const auto loaded = mir::LoadParticleEffect(cache,mir::MakeAssetId("assets/exhaust.particle.json"));
    REQUIRE(loaded.mEffect);
    const auto &emitter = loaded.mEffect->emitterDefinitions[0];
    CHECK(emitter.simulationTarget == mir::ParticleSimulationTarget::CPU);
    CHECK(emitter.script.IsNull());
    CHECK(emitter.scriptCode.empty());
    CHECK(emitter.updateModules[2].parameters[0] == 8);
}
