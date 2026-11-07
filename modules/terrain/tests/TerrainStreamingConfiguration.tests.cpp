/**
 * @file TerrainStreamingConfiguration.tests.cpp
 * @author sumin.park
 * @brief Tests for terrain/TerrainStreamingConfiguration.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include "TerrainPackTestSupport.h"

#include <terrain/TerrainStreamingConfiguration.h>
#include <terrain/TerrainStreamingSettings.h>

#include <core/log/Log.h>
#include <core/log/LogHistory.h>

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
#include <string_view>

using namespace mir;
using namespace mir::test;

namespace
{
    constexpr std::string_view kComplete = R"({
        "cpu": { "budgetMiB": 64 },
        "io": { "workerThreads": 2, "maxInstallsPerFrame": 16 },
        "retry": { "maxAttempts": 3, "firstDelaySeconds": 0.05, "maxDelaySeconds": 0.8, "failedHoldSeconds": 4 },
        "residency": { "mode": "Auto", "queryLeaseSeconds": 2.5 },
        "view": { "refineErrorPixels": 1.5, "mergeErrorPixels": 1.0, "prefetchErrorPixels": 0.5,
                  "prefetchBudgetMiB": 8 },
        "gpu": { "budgetMiB": 256, "stagingMiB": 24, "meshRetainSeconds": 3 },
        "collision": { "spacingsMeters": [8, 4, 2, 1], "maxErrorMeters": 0.05, "pieceTiles": 4,
                       "seamOverlapMeters": 64, "bodyToleranceMeters": 0.1, "loadLatencySeconds": 0.5,
                       "latencyMarginSeconds": 0.25, "prefetchSeconds": 2, "budgetMiB": 48, "retainSeconds": 3 }
    })";

    constexpr std::string_view kLocal = R"({ "cpu": { "budgetMiB": 1536 }, "io": { "workerThreads": 1 } })";

    struct Files
    {
        std::filesystem::path dir = MakeScratchDir("streaming_configuration");
        std::filesystem::path game = dir / "game" / kTerrainStreamingSettingsFileName;
        std::filesystem::path local = dir / "exe" / kTerrainStreamingLocalSettingsFileName;

        Files(std::string_view gameText, std::string_view localText = {})
        {
            std::filesystem::create_directories(game.parent_path());
            std::filesystem::create_directories(local.parent_path());
            REQUIRE(WriteText(game, std::string(gameText)));
            if (!localText.empty())
                REQUIRE(WriteText(local, std::string(localText)));
        }
        ~Files()
        {
            std::filesystem::remove_all(dir);
        }
    };

    // The log history is process-wide, so each test starts it empty.
    struct LogScope
    {
        LogScope()
        {
            InitLog({.filePath = (std::filesystem::temp_directory_path() / "mir_terrain_configuration.log").string(),
                     .consoleLevel = LogLevel::Off});
            LogHistory::Clear();
        }
        ~LogScope()
        {
            ShutdownLog();
        }
    };

    nlohmann::json Complete()
    {
        return nlohmann::json::parse(kComplete);
    }

    void CheckSame(const TerrainStreamingSettings &a, const TerrainStreamingSettings &b)
    {
        CHECK(a.cpu.budgetBytes == b.cpu.budgetBytes);
        CHECK(a.io.workerThreads == b.io.workerThreads);
        CHECK(a.io.maxInstallsPerFrame == b.io.maxInstallsPerFrame);
        CHECK(a.retry.maxAttempts == b.retry.maxAttempts);
        CHECK(a.residency.mode == b.residency.mode);
        CHECK(a.view.prefetchBudgetBytes == b.view.prefetchBudgetBytes);
        CHECK(a.gpu.budgetBytes == b.gpu.budgetBytes);
        CHECK(a.gpu.stagingBytes == b.gpu.stagingBytes);
        CHECK(a.collision.spacingsMeters == b.collision.spacingsMeters);
        CHECK(a.collision.pieceTiles == b.collision.pieceTiles);
        CHECK(a.collision.budgetBytes == b.collision.budgetBytes);
    }
} // namespace

TEST_CASE("The configuration publishes the same value as LoadTerrainStreamingSettings",
          "[terrain][configuration]")
{
    Files files(kComplete, kLocal);
    const TerrainStreamingSettingsLoad load = LoadTerrainStreamingSettings(files.game, files.local);
    REQUIRE(load.settings);

    const TerrainStreamingConfiguration config(files.game, files.local);
    REQUIRE(config.Effective());
    CheckSame(*config.Effective(), *load.settings);
    CHECK(config.Effective()->cpu.budgetBytes == uint64_t(1536) * 1024 * 1024);
    CHECK(config.Problems().empty());
    CHECK(config.GameFileFound());
    CHECK(config.LocalFileFound());
}

TEST_CASE("An unchanged project refreshes to false", "[terrain][configuration]")
{
    Files files(kComplete, kLocal);
    TerrainStreamingConfiguration config(files.game, files.local);
    REQUIRE(config.Effective());
    CHECK_FALSE(config.Refresh());
    CHECK_FALSE(config.Refresh());
}

TEST_CASE("A valid project change publishes and the local override still wins", "[terrain][configuration]")
{
    Files files(kComplete, kLocal);
    TerrainStreamingConfiguration config(files.game, files.local);
    REQUIRE(config.Effective());

    config.Project()["gpu"]["budgetMiB"] = 512;
    config.Project()["cpu"]["budgetMiB"] = 128;
    REQUIRE(config.Refresh());
    REQUIRE(config.Effective());
    CHECK(config.Effective()->gpu.budgetBytes == uint64_t(512) * 1024 * 1024);
    CHECK(config.Effective()->cpu.budgetBytes == uint64_t(1536) * 1024 * 1024);
    CHECK(config.Problems().empty());
    CHECK_FALSE(config.Refresh());
}

TEST_CASE("A change the local override masks is recorded and leaves Effective equal", "[terrain][configuration]")
{
    Files files(kComplete, kLocal);
    TerrainStreamingConfiguration config(files.game, files.local);
    REQUIRE(config.Effective());
    const TerrainStreamingSettings before = *config.Effective();

    config.Project()["cpu"]["budgetMiB"] = 128;
    CHECK(config.Refresh());
    REQUIRE(config.Effective());
    CheckSame(*config.Effective(), before);
    CHECK_FALSE(config.Refresh());
}

TEST_CASE("An invalid change keeps the previous value and logs once", "[terrain][configuration]")
{
    LogScope log;
    Files files(kComplete);
    TerrainStreamingConfiguration config(files.game, files.local);
    REQUIRE(config.Effective());
    const TerrainStreamingSettings before = *config.Effective();

    config.Project()["cpu"]["budgetMiB"] = 0;
    CHECK_FALSE(config.Refresh());
    REQUIRE(config.Effective());
    CheckSame(*config.Effective(), before);
    CHECK_FALSE(config.Problems().empty());
    const size_t logged = LogHistory::Snapshot().size();
    CHECK(logged == 1);

    CHECK_FALSE(config.Refresh());
    CHECK(LogHistory::Snapshot().size() == logged);
    CHECK_FALSE(config.Problems().empty());
}

TEST_CASE("Fixing an invalid project publishes on the next refresh", "[terrain][configuration]")
{
    LogScope log;
    Files files(kComplete);
    TerrainStreamingConfiguration config(files.game, files.local);
    REQUIRE(config.Effective());

    config.Project()["cpu"]["budgetMiB"] = 0;
    CHECK_FALSE(config.Refresh());
    config.Project()["cpu"]["budgetMiB"] = 32;
    REQUIRE(config.Refresh());
    REQUIRE(config.Effective());
    CHECK(config.Effective()->cpu.budgetBytes == uint64_t(32) * 1024 * 1024);
    CHECK(config.Problems().empty());
}

TEST_CASE("An invalid game file at construction leaves Effective empty with the problems stored",
          "[terrain][configuration]")
{
    LogScope log;
    Files files("[1, 2]");
    TerrainStreamingConfiguration config(files.game, files.local);
    CHECK_FALSE(config.Effective());
    CHECK_FALSE(config.Problems().empty());
    CHECK(config.GameFileFound());
    CHECK_FALSE(config.LocalFileFound());
    CHECK(LogHistory::Snapshot().empty());
    CHECK(config.Project().is_null());
    CHECK_FALSE(config.Refresh());

    config.Project() = Complete();
    CHECK(config.Refresh());
    CHECK(config.Effective());
    CHECK(config.Problems().empty());
}

TEST_CASE("A broken local file still blocks a later project change", "[terrain][configuration]")
{
    LogScope log;
    Files files(kComplete, "[1]");
    TerrainStreamingConfiguration config(files.game, files.local);
    CHECK_FALSE(config.Effective());
    CHECK_FALSE(config.Problems().empty());

    // The complete file already says 64, so a different value is what makes the project differ.
    config.Project()["cpu"]["budgetMiB"] = 32;
    CHECK_FALSE(config.Refresh());
    CHECK_FALSE(config.Effective());
    REQUIRE_FALSE(config.Problems().empty());
    CHECK(config.Problems().front().find(kTerrainStreamingLocalSettingsFileName) != std::string::npos);
}
