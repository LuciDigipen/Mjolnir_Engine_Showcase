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
        }
        ~CookFixture()
        {
            std::error_code error;
            if (std::filesystem::weakly_canonical(root, error).parent_path() == parent && !error)
                std::filesystem::remove_all(root, error);
        }
        int Cook() const
        {
            const std::vector<std::string> args{
                "--source-dir", source.string(), "--id-root", "assets", "--out", out.string()};
            return mir::LaunchProcess(GLTF_COOKER_EXE, args, true).value_or(-1);
        }
    };
}

TEST_CASE("Particle preset cooks and loads with a typed texture through the real asset pipeline", "[particles][cooker]")
{
    CookFixture fixture;
    REQUIRE(fixture.Cook() == 0);
    auto manifest = mir::AssetManifest::LoadFile(fixture.out / mir::kAssetManifestFileName);
    REQUIRE(manifest);
    CHECK(manifest->Count() == 2);
    const auto id = mir::MakeAssetId("assets/exhaust.particle.json");
    const auto *entry = manifest->Find(id);
    REQUIRE(entry);
    CHECK(entry->typeTag == mir::kParticleEffectTypeTag);
    CHECK(entry->contentVersion == mir::kParticleEffectContentVersion);
    mir::AssetCache cache(&*manifest, fixture.out);
    const auto loaded = mir::LoadParticleEffect(cache, id);
    REQUIRE(loaded.mEffect);
    CHECK(loaded.mEffect->texture == mir::MakeAssetId("assets/checker.png"));
    CHECK(loaded.mEffect->spawnRate == 60.0f);

    const auto oldManifest = mir::ReadBytes(fixture.out / mir::kAssetManifestFileName);
    std::filesystem::remove(fixture.source / "checker.png");
    CHECK(fixture.Cook() != 0);
    CHECK(mir::ReadBytes(fixture.out / mir::kAssetManifestFileName) == oldManifest);
}

TEST_CASE("Particle cooker rejects unsupported source versions before publishing a manifest", "[particles][cooker]")
{
    CookFixture fixture;
    std::ofstream(fixture.source / "exhaust.particle.json") <<
        R"({"version":2,"lifetime":2,"spawnRate":60,"capacity":256,"texture":"checker.png"})";
    CHECK(fixture.Cook() != 0);
    CHECK_FALSE(std::filesystem::exists(fixture.out / mir::kAssetManifestFileName));
}
