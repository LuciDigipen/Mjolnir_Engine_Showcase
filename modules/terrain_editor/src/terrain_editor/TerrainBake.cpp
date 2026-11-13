/**
 * @file TerrainBake.cpp
 * @author Jackey Cen
 * @brief The TerrainTool sync command the panel runs, its report, and the rule that drops a baked overlay.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <terrain_editor/TerrainBake.h>

#include <core/fs/FileIO.h>
#include <core/fs/Paths.h>
#include <terrain/TerrainAssetPath.h>
#include <terrain/TerrainRelease.h>

#include <nlohmann/json.hpp>

#include <format>
#include <optional>

namespace mir
{
    namespace
    {
        namespace fs = std::filesystem;
        using Json = nlohmann::json;

        std::string PathText(const fs::path &path)
        {
            const std::u8string text = path.generic_u8string();
            return { reinterpret_cast<const char *>(text.data()), text.size() };
        }

        std::string Text(const Json &json, std::string_view key)
        {
            const auto it = json.find(std::string(key));
            return it != json.end() && it->is_string() ? it->get<std::string>() : std::string();
        }

        std::optional<uint64_t> IdOf(const Json &json, std::string_view key)
        {
            const auto it = json.find(std::string(key));
            if (it == json.end() || !it->is_string())
                return std::nullopt;
            return ParseTerrainIdHex(it->get<std::string>());
        }

        std::string FirstDiagnostic(const Json &json)
        {
            const auto diagnostics = json.find("diagnostics");
            if (diagnostics == json.end() || !diagnostics->is_array())
                return {};

            for (const Json &diagnostic : *diagnostics)
            {
                if (!diagnostic.is_object())
                    continue;
                const std::string message = Text(diagnostic, "message");
                if (message.empty())
                    continue;
                const std::string check = Text(diagnostic, "check");
                return check.empty() ? message : std::format("[{}] {}", check, message);
            }
            return {};
        }
    }

    std::optional<TerrainBakeRequest> MakeTerrainBakeRequest(const TerrainBakeProject &project,
        std::string_view recipePath, std::string &error)
    {
        const TerrainAssetPath path = ResolveTerrainAssetPath(project.mIdRoot, recipePath);
        if (!path.Ok())
        {
            error = DescribeTerrainAssetPathError(path, project.mIdRoot, recipePath);
            return std::nullopt;
        }

        // Sync refuses a candidate inside the releases tree, so the editor's scratch files sit beside it.
        const fs::path workDir =
            TerrainReleasesRoot(project.mGameDir).parent_path() / "editor-bake" / TerrainIdHex(path.asset.value);

        TerrainBakeRequest request;
        request.mTool = project.mTool;
        request.mRecipe = project.mAssetsDir / fs::path(std::string(recipePath));
        request.mIdRoot = project.mIdRoot;
        request.mSourcePath = std::string(recipePath);
        request.mGameDir = project.mGameDir;
        request.mCandidateDir = workDir / "candidate";
        request.mReportFile = workDir / "sync.json";
        request.mAsset = path.asset;
        return request;
    }

    std::vector<std::string> TerrainBakeArguments(const TerrainBakeRequest &request)
    {
        return { "sync", "--recipe", PathText(request.mRecipe), "--id-root", request.mIdRoot, "--source-path",
            request.mSourcePath, "--game-dir", PathText(request.mGameDir), "--candidate",
            PathText(request.mCandidateDir), "--report", PathText(request.mReportFile) };
    }

    std::filesystem::path ReadTerrainToolPath(const std::filesystem::path &workspaceFile)
    {
        const std::optional<std::string> text = ReadText(workspaceFile);
        if (!text)
            return {};

        const Json json = Json::parse(*text, nullptr, false);
        if (json.is_discarded() || !json.is_object())
            return {};

        const std::string tool = Text(json, "terrainTool");
        return tool.empty() ? fs::path() : fs::path(std::u8string(tool.begin(), tool.end()));
    }

    std::filesystem::path TerrainToolPath()
    {
        return ReadTerrainToolPath(ExecutableDir() / "asset_workspace.json");
    }

    TerrainBakeReport ReadTerrainBakeReport(const std::filesystem::path &reportFile, int exitCode)
    {
        TerrainBakeReport report;
        const std::optional<std::string> text = ReadText(reportFile);
        if (!text)
        {
            report.mMessage = std::format("TerrainTool exited with code {} and wrote no report", exitCode);
            return report;
        }

        const Json json = Json::parse(*text, nullptr, false);
        if (json.is_discarded() || !json.is_object())
        {
            report.mMessage = "TerrainTool wrote an unreadable report";
            return report;
        }

        if (const auto recipe = json.find("recipe"); recipe != json.end() && recipe->is_object())
        {
            if (const std::optional<uint64_t> fingerprint = IdOf(*recipe, "fingerprint"))
                report.mFingerprint = TerrainSourceFingerprint(*fingerprint);
        }

        const auto publish = json.find("publish");
        if (json.value("ok", false) && publish != json.end() && publish->is_object())
        {
            if (const std::optional<uint64_t> current = IdOf(*publish, "current"))
            {
                report.mRelease = TerrainReleaseId(*current);
                report.mOutcome =
                    publish->value("published", false) ? TerrainBakeOutcome::Published : TerrainBakeOutcome::UpToDate;
                return report;
            }
        }

        report.mOutcome = TerrainBakeOutcome::Failed;
        report.mMessage = FirstDiagnostic(json);
        if (report.mMessage.empty())
            report.mMessage = std::format("TerrainTool sync failed (exit code {})", exitCode);
        return report;
    }

    TerrainBakeOverlayAction EvaluateTerrainBakeDiscard(const TerrainBakeReport &report, uint64_t startChange,
        uint64_t currentChange, const TerrainReleaseInfo *loaded)
    {
        if (startChange != currentChange)
            return TerrainBakeOverlayAction::Keep;

        switch (report.mOutcome)
        {
        case TerrainBakeOutcome::Published:
            if (!report.mRelease || loaded == nullptr || !(loaded->release == *report.mRelease))
                return TerrainBakeOverlayAction::Wait;
            return TerrainBakeOverlayAction::Discard;
        case TerrainBakeOutcome::UpToDate:
            if (!report.mFingerprint || loaded == nullptr)
                return TerrainBakeOverlayAction::Wait;
            return loaded->source == *report.mFingerprint ? TerrainBakeOverlayAction::Discard
                                                         : TerrainBakeOverlayAction::Keep;
        case TerrainBakeOutcome::Canceled:
        case TerrainBakeOutcome::Failed:
            return TerrainBakeOverlayAction::Keep;
        }
        return TerrainBakeOverlayAction::Keep;
    }
} // namespace mir
