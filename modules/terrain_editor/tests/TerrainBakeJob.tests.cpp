/**
 * @file TerrainBakeJob.tests.cpp
 * @author Jackey Cen
 * @brief Tests for terrain_editor/TerrainBakeJob with a fake process.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <terrain_editor/TerrainBakeJob.h>

#include <terrain_editor/TerrainLayerDocument.h>

#include <assets/AssetId.h>
#include <terrain/TerrainEditLayer.h>
#include <terrain/TerrainEditStore.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    namespace fs = std::filesystem;
    using mir::AssetId;
    using mir::SaveTerrainLayersForBake;
    using mir::TerrainBakeJob;
    using mir::TerrainBakeOutcome;
    using mir::TerrainBakePrepare;
    using mir::TerrainBakeProcess;
    using mir::TerrainBakeRequest;
    using mir::TerrainBakeStart;
    using mir::TerrainEditBlend;
    using mir::TerrainEditLayer;
    using mir::TerrainEditLayersDir;
    using mir::TerrainEditLayerInfo;
    using mir::TerrainLayerDocument;

    class FakeProcess final : public TerrainBakeProcess
    {
    public:
        bool Start(const std::filesystem::path &program, const std::vector<std::string> &arguments) override
        {
            mProgram = program;
            mArguments = arguments;
            mStarted = true;
            return mStartResult;
        }

        std::optional<int> Poll() override { return mExitCode; }
        void Terminate() override { mTerminated = true; }
        fs::path ReportPath() const { return fs::u8path(mArguments.back()); }

        std::filesystem::path mProgram;
        std::vector<std::string> mArguments;
        std::optional<int> mExitCode;
        bool mStartResult = true;
        bool mStarted = false;
        bool mTerminated = false;
    };

    struct FakeGroup
    {
        std::unique_ptr<TerrainBakeProcess> Make()
        {
            ++mCreated;
            auto process = std::make_unique<FakeProcess>();
            process->mStartResult = mStartResult;
            mLast = process.get();
            return process;
        }

        int mCreated = 0;
        bool mStartResult = true;
        FakeProcess *mLast = nullptr;
    };

    fs::path MakeScratchDir(std::string_view tag)
    {
        static std::random_device random;
        const fs::path dir = fs::temp_directory_path() / "mir_terrain_bake_job_tests" /
            (std::string(tag) + "_" + std::to_string(random()));
        fs::remove_all(dir);
        fs::create_directories(dir);
        return dir;
    }

    TerrainBakeRequest MakeRequest(const fs::path &root)
    {
        TerrainBakeRequest request;
        request.mTool = root / "TerrainTool.exe";
        request.mRecipe = root / "assets" / "terrain" / "hills.terrain.json";
        request.mIdRoot = "games/Test/assets";
        request.mSourcePath = "terrain/hills.terrain.json";
        request.mGameDir = root / "games" / "Test";
        request.mCandidateDir = root / "releases" / "editor-bake" / "candidate";
        request.mReportDirectory = root / "releases" / "editor-bake";
        request.mAsset = AssetId{ 0x1234 };
        return request;
    }

    bool StartJob(TerrainBakeJob &job, const TerrainBakeRequest &request, uint64_t change = 0,
        TerrainBakePrepare prepare = {})
    {
        TerrainBakeStart start;
        start.mRequest = request;
        start.mStartChange = change;
        start.mPrepare = std::move(prepare);
        return job.Start(std::move(start));
    }

    void WriteReport(const fs::path &path, std::string_view text)
    {
        fs::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
    }

    TerrainEditLayer EmptyLayer(std::string name, uint32_t sequence)
    {
        TerrainEditLayer layer;
        layer.info = TerrainEditLayerInfo{ std::move(name), 0, sequence, true, TerrainEditBlend::Additive };
        return layer;
    }
} // namespace

TEST_CASE("the layers are saved before TerrainTool starts")
{
    const fs::path root = MakeScratchDir("save");
    TerrainLayerDocument document(TerrainEditLayersDir(root / "hills.terrain.json"));
    document.Load();
    document.Append(EmptyLayer("Ridge", 1));
    REQUIRE(document.Unsaved());

    const TerrainBakeRequest request = MakeRequest(root);
    FakeGroup group;
    TerrainBakeJob job([&group] { return group.Make(); });

    const bool started = StartJob(job, request, 4, SaveTerrainLayersForBake(document));

    CHECK(started);
    CHECK_FALSE(document.Unsaved());
    CHECK(job.Running());
    CHECK(job.Seconds() >= 0.0);
    REQUIRE(group.mLast);
    CHECK(group.mLast->mStarted);
    CHECK(group.mLast->mProgram == request.mTool);
    CHECK(group.mLast->ReportPath().parent_path().parent_path() == request.mReportDirectory);
    CHECK(group.mLast->mArguments == mir::TerrainBakeArguments(request, group.mLast->ReportPath()));
}

TEST_CASE("a failed save keeps TerrainTool from starting")
{
    const fs::path root = MakeScratchDir("save-failed");
    TerrainLayerDocument document(TerrainEditLayersDir(root / "hills.terrain.json"));
    document.Load();
    document.Append(EmptyLayer("Ridge", 1));
    document.Append(EmptyLayer("Path", 2));
    REQUIRE(document.Save());

    TerrainEditLayerInfo duplicate = document.Layers()[1].info;
    duplicate.sequence = document.Layers()[0].info.sequence;
    document.SetInfo(1, duplicate);
    REQUIRE(document.Unsaved());

    const TerrainBakeRequest request = MakeRequest(root);
    FakeGroup group;
    TerrainBakeJob job([&group] { return group.Make(); });

    const bool started = StartJob(job, request, 1, SaveTerrainLayersForBake(document));

    CHECK_FALSE(started);
    CHECK(document.Unsaved());
    CHECK_FALSE(job.Running());
    CHECK(group.mCreated == 0);
    CHECK(job.Completed() == 1);
    REQUIRE(job.Result());
    CHECK(job.Result()->mReport.mOutcome == TerrainBakeOutcome::Failed);
    CHECK_FALSE(job.Result()->mReport.mMessage.empty());
    CHECK(job.Result()->mReport.mMessage == document.SaveError());
}

TEST_CASE("only one bake runs at a time")
{
    const TerrainBakeRequest request = MakeRequest(MakeScratchDir("one-at-a-time"));
    FakeGroup group;
    TerrainBakeJob job([&group] { return group.Make(); });

    REQUIRE(StartJob(job, request));
    CHECK_FALSE(StartJob(job, request));
    CHECK(group.mCreated == 1);
}

TEST_CASE("cancel terminates the process and reports canceled")
{
    const TerrainBakeRequest request = MakeRequest(MakeScratchDir("cancel"));
    FakeGroup group;
    TerrainBakeJob job([&group] { return group.Make(); });

    REQUIRE(StartJob(job, request));
    REQUIRE(group.mLast);

    job.Cancel();
    CHECK(group.mLast->mTerminated);
    CHECK_FALSE(job.Running());
    REQUIRE(job.Result());
    CHECK(job.Result()->mReport.mOutcome == TerrainBakeOutcome::Canceled);
}

TEST_CASE("a published report ends the bake as published")
{
    const fs::path root = MakeScratchDir("published");
    const TerrainBakeRequest request = MakeRequest(root);
    FakeGroup group;
    TerrainBakeJob job([&group] { return group.Make(); });
    REQUIRE(StartJob(job, request, 9));
    REQUIRE(group.mLast);
    WriteReport(group.mLast->ReportPath(),
        R"({"kind":"mir.terrain.report","command":"sync","ok":true,"recipe":{"path":"terrain/hills.terrain.json","fingerprint":"00000000000000b2"},"publish":{"releases":"C:/Releases","previous":null,"current":"00000000000000a1","published":true},"diagnostics":[]})");

    group.mLast->mExitCode = 0;
    job.Update();

    CHECK_FALSE(job.Running());
    REQUIRE(job.Result());
    CHECK(job.Result()->mReport.mOutcome == TerrainBakeOutcome::Published);
    REQUIRE(job.Result()->mReport.mRelease);
    CHECK(job.Result()->mReport.mRelease->value == 0xa1);
    CHECK(job.Result()->mStartChange == 9);
    CHECK(job.Result()->mAsset == request.mAsset);
}

TEST_CASE("an up to date report ends the bake as up to date")
{
    const fs::path root = MakeScratchDir("up-to-date");
    const TerrainBakeRequest request = MakeRequest(root);
    FakeGroup group;
    TerrainBakeJob job([&group] { return group.Make(); });
    REQUIRE(StartJob(job, request));
    REQUIRE(group.mLast);

    WriteReport(group.mLast->ReportPath(),
        R"({"kind":"mir.terrain.report","command":"sync","ok":true,"recipe":{"path":"terrain/hills.terrain.json","fingerprint":"00000000000000b2"},"publish":{"releases":"C:/Releases","previous":null,"current":"00000000000000a1","published":false},"diagnostics":[]})");
    group.mLast->mExitCode = 0;
    job.Update();

    REQUIRE(job.Result());
    CHECK(job.Result()->mReport.mOutcome == TerrainBakeOutcome::UpToDate);
    REQUIRE(job.Result()->mReport.mRelease);
    CHECK(job.Result()->mReport.mRelease->value == 0xa1);
}

TEST_CASE("a failed report ends the bake with its first diagnostic")
{
    const fs::path root = MakeScratchDir("failed");
    const TerrainBakeRequest request = MakeRequest(root);
    FakeGroup group;
    TerrainBakeJob job([&group] { return group.Make(); });
    REQUIRE(StartJob(job, request));
    REQUIRE(group.mLast);

    WriteReport(group.mLast->ReportPath(),
        R"({"kind":"mir.terrain.report","command":"sync","ok":false,"publish":null,"diagnostics":[{"check":"pixels","field":"","tiles":[],"sample":null,"message":"pack is damaged"}]})");
    group.mLast->mExitCode = 1;
    job.Update();

    REQUIRE(job.Result());
    CHECK(job.Result()->mReport.mOutcome == TerrainBakeOutcome::Failed);
    CHECK(job.Result()->mReport.mMessage.find("pack is damaged") != std::string::npos);
}

TEST_CASE("a missing report ends the bake as failed")
{
    const TerrainBakeRequest request = MakeRequest(MakeScratchDir("missing-report"));
    FakeGroup group;
    TerrainBakeJob job([&group] { return group.Make(); });
    REQUIRE(StartJob(job, request));
    REQUIRE(group.mLast);

    group.mLast->mExitCode = 0;
    job.Update();

    REQUIRE(job.Result());
    CHECK(job.Result()->mReport.mOutcome == TerrainBakeOutcome::Failed);
    CHECK(job.Result()->mReport.mMessage.find("no report") != std::string::npos);
}

TEST_CASE("a sync cannot use another run's success report for the same terrain")
{
    const auto request = MakeRequest(MakeScratchDir("report-isolation"));
    FakeGroup first, second;
    TerrainBakeJob writer([&first] { return first.Make(); });
    TerrainBakeJob reader([&second] { return second.Make(); });
    REQUIRE(StartJob(writer, request));
    REQUIRE(StartJob(reader, request));
    const auto previous = first.mLast->ReportPath();
    const auto current = second.mLast->ReportPath();
    REQUIRE(previous != current);
    WriteReport(previous, R"({"ok":true,"publish":{"current":"00000000000000a1","published":true}})");
    second.mLast->mExitCode = 0;
    reader.Update();
    REQUIRE(reader.Result());
    CHECK(reader.Result()->mReport.mOutcome == TerrainBakeOutcome::Failed);
    CHECK(fs::is_regular_file(previous));
    first.mLast->mExitCode = 0;
    writer.Update();
    REQUIRE(writer.Result());
    CHECK(writer.Result()->mReport.mOutcome == TerrainBakeOutcome::Published);
    CHECK_FALSE(fs::exists(previous));
    REQUIRE(StartJob(reader, request));
    CHECK(second.mLast->ReportPath() != current);
}

TEST_CASE("a TerrainTool that cannot start ends the bake as failed")
{
    const TerrainBakeRequest request = MakeRequest(MakeScratchDir("start-failed"));
    FakeGroup group;
    group.mStartResult = false;
    TerrainBakeJob job([&group] { return group.Make(); });

    const bool started = StartJob(job, request);

    CHECK_FALSE(started);
    CHECK_FALSE(job.Running());
    REQUIRE(job.Result());
    CHECK(job.Result()->mReport.mOutcome == TerrainBakeOutcome::Failed);
    CHECK(job.Result()->mReport.mMessage.find("could not start TerrainTool") != std::string::npos);
}
