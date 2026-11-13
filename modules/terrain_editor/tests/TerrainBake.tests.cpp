/**
 * @file TerrainBake.tests.cpp
 * @author Jackey Cen
 * @brief Tests for terrain_editor/TerrainBake: arguments, report parsing and the overlay discard rule.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <terrain_editor/TerrainBake.h>

#include <terrain/TerrainAssetPath.h>
#include <terrain/TerrainIdentity.h>
#include <terrain/TerrainRelease.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    namespace fs = std::filesystem;
    using mir::AssetId;
    using mir::EvaluateTerrainBakeDiscard;
    using mir::MakeTerrainBakeRequest;
    using mir::ReadTerrainBakeReport;
    using mir::ReadTerrainToolPath;
    using mir::ResolveTerrainAssetPath;
    using mir::TerrainAssetPath;
    using mir::TerrainBakeOutcome;
    using mir::TerrainBakeOverlayAction;
    using mir::TerrainBakeProject;
    using mir::TerrainBakeReport;
    using mir::TerrainBakeRequest;
    using mir::TerrainReleaseId;
    using mir::TerrainReleaseInfo;
    using mir::TerrainReleasesRoot;
    using mir::TerrainSourceFingerprint;
    using mir::TerrainIdHex;

    fs::path MakeScratchDir(std::string_view tag)
    {
        static std::random_device random;
        const fs::path dir = fs::temp_directory_path() / "mir_terrain_bake_tests" /
            (std::string(tag) + "_" + std::to_string(random()));
        fs::remove_all(dir);
        fs::create_directories(dir);
        return dir;
    }

    std::string Utf8(const fs::path &path)
    {
        const std::u8string text = path.generic_u8string();
        return { reinterpret_cast<const char *>(text.data()), text.size() };
    }

    TerrainBakeProject MakeProject(const fs::path &root)
    {
        TerrainBakeProject project;
        project.mGameDir = root / "games" / "Test";
        project.mAssetsDir = project.mGameDir / "assets";
        project.mIdRoot = "games/Test/assets";
        project.mTool = root / "TerrainTool.exe";
        return project;
    }

    void WriteFile(const fs::path &path, std::string_view text)
    {
        fs::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
    }

    std::string ReportText(std::string_view publish, std::string_view diagnostics = "[]")
    {
        return std::string("{\"kind\":\"mir.terrain.report\",\"command\":\"sync\",\"ok\":true,") +
            "\"recipe\":{\"path\":\"hills.terrain.json\",\"fingerprint\":\"00000000000000b2\"},\"publish\":" +
            std::string(publish) + ",\"diagnostics\":" + std::string(diagnostics) + "}";
    }

    TerrainBakeReport PublishedReport(TerrainReleaseId release)
    {
        TerrainBakeReport report;
        report.mOutcome = TerrainBakeOutcome::Published;
        report.mRelease = release;
        return report;
    }

    TerrainReleaseInfo LoadedRelease(TerrainReleaseId release, TerrainSourceFingerprint source)
    {
        TerrainReleaseInfo info;
        info.asset = AssetId{ 0x1234 };
        info.source = source;
        info.release = release;
        return info;
    }
} // namespace

TEST_CASE("sync arguments match the ones the build runs")
{
    const fs::path root = MakeScratchDir("arguments");
    const TerrainBakeProject project = MakeProject(root);

    std::string error;
    const std::optional<TerrainBakeRequest> request =
        MakeTerrainBakeRequest(project, "terrain/hills.terrain.json", error);
    REQUIRE(request);
    CHECK(error.empty());

    const TerrainAssetPath runtime = ResolveTerrainAssetPath(project.mIdRoot, "terrain/hills.terrain.json");
    REQUIRE(runtime.Ok());
    CHECK(request->mAsset == runtime.asset);

    const fs::path workDir =
        TerrainReleasesRoot(project.mGameDir).parent_path() / "editor-bake" / TerrainIdHex(runtime.asset.value);
    const std::vector<std::string> expected = {
        "sync",
        "--recipe",
        Utf8(project.mAssetsDir / "terrain/hills.terrain.json"),
        "--id-root",
        "games/Test/assets",
        "--source-path",
        "terrain/hills.terrain.json",
        "--game-dir",
        Utf8(project.mGameDir),
        "--candidate",
        Utf8(workDir / "candidate"),
        "--report",
        Utf8(workDir / "sync.json"),
    };
    CHECK(mir::TerrainBakeArguments(*request) == expected);

    const fs::path releases = TerrainReleasesRoot(project.mGameDir);
    const fs::path relative = request->mCandidateDir.lexically_normal().lexically_relative(releases.lexically_normal());
    CHECK((relative.empty() || *relative.begin() == ".."));
}

TEST_CASE("a recipe path outside the id root is refused")
{
    const TerrainBakeProject project = MakeProject(MakeScratchDir("refused"));

    std::string error;
    CHECK_FALSE(MakeTerrainBakeRequest(project, "../hills.terrain.json", error));
    CHECK_FALSE(error.empty());
}

TEST_CASE("the TerrainTool path comes from the asset workspace binding")
{
    const fs::path root = MakeScratchDir("tool-path");
    const fs::path workspace = root / "asset_workspace.json";
    WriteFile(workspace, R"({"version":1,"sourceRoot":"C:/Source","terrainTool":"C:/Tools/TerrainTool.exe"})");

    CHECK(ReadTerrainToolPath(workspace) == fs::path("C:/Tools/TerrainTool.exe"));

    WriteFile(workspace, R"({"version":1,"sourceRoot":"C:/Source"})");
    CHECK(ReadTerrainToolPath(workspace).empty());

    fs::remove(workspace);
    CHECK(ReadTerrainToolPath(workspace).empty());
}

TEST_CASE("a published report names the release and the recipe")
{
    const fs::path root = MakeScratchDir("report-published");
    const fs::path reportFile = root / "sync.json";
    WriteFile(reportFile, ReportText(R"({"releases":"C:/R","previous":null,"current":"00000000000000a1","published":true})"));

    const TerrainBakeReport report = ReadTerrainBakeReport(reportFile, 0);
    CHECK(report.mOutcome == TerrainBakeOutcome::Published);
    REQUIRE(report.mRelease);
    CHECK(report.mRelease->value == 0xa1);
    REQUIRE(report.mFingerprint);
    CHECK(report.mFingerprint->value == 0xb2);
}

TEST_CASE("the report decides when there is one, even on a lost exit code")
{
    const fs::path root = MakeScratchDir("report-up-to-date");
    const fs::path reportFile = root / "sync.json";
    WriteFile(reportFile,
        ReportText(R"({"releases":"C:/R","previous":null,"current":"00000000000000a1","published":false})"));

    const TerrainBakeReport report = ReadTerrainBakeReport(reportFile, 1);
    CHECK(report.mOutcome == TerrainBakeOutcome::UpToDate);
    REQUIRE(report.mRelease);
    CHECK(report.mRelease->value == 0xa1);
}

TEST_CASE("a failed report shows its first diagnostic")
{
    const fs::path root = MakeScratchDir("report-failed");
    const fs::path reportFile = root / "sync.json";
    WriteFile(reportFile,
        R"({"kind":"mir.terrain.report","command":"sync","ok":false,"publish":null,"diagnostics":[{"check":"pixels","field":"","tiles":[],"sample":null,"message":"pack is damaged"},{"check":"other","field":"","tiles":[],"sample":null,"message":"later"}]})");

    const TerrainBakeReport report = ReadTerrainBakeReport(reportFile, 1);
    CHECK(report.mOutcome == TerrainBakeOutcome::Failed);
    CHECK(report.mMessage.find("pack is damaged") != std::string::npos);
    CHECK(report.mMessage.find("later") == std::string::npos);
}

TEST_CASE("a missing or unreadable report is a failure")
{
    const fs::path root = MakeScratchDir("report-missing");
    const fs::path reportFile = root / "sync.json";

    const TerrainBakeReport missing = ReadTerrainBakeReport(reportFile, 1);
    CHECK(missing.mOutcome == TerrainBakeOutcome::Failed);
    CHECK(missing.mMessage.find("no report") != std::string::npos);

    WriteFile(reportFile, "not json");
    const TerrainBakeReport unreadable = ReadTerrainBakeReport(reportFile, 0);
    CHECK(unreadable.mOutcome == TerrainBakeOutcome::Failed);
    CHECK(unreadable.mMessage.find("unreadable") != std::string::npos);
}

TEST_CASE("the overlay is dropped only once the asset loaded the published release")
{
    const TerrainBakeReport report = PublishedReport(TerrainReleaseId{ 0xa1 });
    const TerrainReleaseInfo older = LoadedRelease(TerrainReleaseId{ 0xa0 }, TerrainSourceFingerprint{ 1 });
    const TerrainReleaseInfo published = LoadedRelease(TerrainReleaseId{ 0xa1 }, TerrainSourceFingerprint{ 1 });

    CHECK(EvaluateTerrainBakeDiscard(report, 3, 3, nullptr) == TerrainBakeOverlayAction::Wait);
    CHECK(EvaluateTerrainBakeDiscard(report, 3, 3, &older) == TerrainBakeOverlayAction::Wait);
    CHECK(EvaluateTerrainBakeDiscard(report, 3, 4, &published) == TerrainBakeOverlayAction::Keep);
    CHECK(EvaluateTerrainBakeDiscard(report, 3, 3, &published) == TerrainBakeOverlayAction::Discard);
}

TEST_CASE("an up to date bake keeps the overlay until the release carries the recipe")
{
    TerrainBakeReport report;
    report.mOutcome = TerrainBakeOutcome::UpToDate;
    report.mRelease = TerrainReleaseId{ 0xa1 };
    report.mFingerprint = TerrainSourceFingerprint{ 7 };

    const TerrainReleaseInfo other = LoadedRelease(TerrainReleaseId{ 0xa1 }, TerrainSourceFingerprint{ 8 });
    const TerrainReleaseInfo same = LoadedRelease(TerrainReleaseId{ 0xa1 }, TerrainSourceFingerprint{ 7 });

    CHECK(EvaluateTerrainBakeDiscard(report, 2, 2, nullptr) == TerrainBakeOverlayAction::Wait);
    CHECK(EvaluateTerrainBakeDiscard(report, 2, 2, &other) == TerrainBakeOverlayAction::Keep);
    CHECK(EvaluateTerrainBakeDiscard(report, 2, 3, &same) == TerrainBakeOverlayAction::Keep);
    CHECK(EvaluateTerrainBakeDiscard(report, 2, 2, &same) == TerrainBakeOverlayAction::Discard);
}

TEST_CASE("a failed or canceled bake keeps the overlay")
{
    const TerrainReleaseInfo loaded = LoadedRelease(TerrainReleaseId{ 0xa1 }, TerrainSourceFingerprint{ 1 });

    TerrainBakeReport failed;
    failed.mOutcome = TerrainBakeOutcome::Failed;
    CHECK(EvaluateTerrainBakeDiscard(failed, 1, 1, &loaded) == TerrainBakeOverlayAction::Keep);

    TerrainBakeReport canceled;
    canceled.mOutcome = TerrainBakeOutcome::Canceled;
    CHECK(EvaluateTerrainBakeDiscard(canceled, 1, 1, &loaded) == TerrainBakeOverlayAction::Keep);
}
