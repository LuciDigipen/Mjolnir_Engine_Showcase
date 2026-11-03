/**
 * @file BuildSettings.cpp
 * @author Quy Dong Ta
 * @brief File > Build Settings: the profile shared in project_settings.json, and the
 *        per-machine template and output paths resolved against the checkout.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <editor/BuildSettings.h>

#include <project/PlayerInfo.h>
#include <serial/JsonFile.h>

#include <optional>
#include <utility>

namespace mir
{
    namespace
    {
        using json = nlohmann::json;

        std::filesystem::path FromCheckout(
            const std::string &text, const std::filesystem::path &sourceRoot, const std::filesystem::path &fallback)
        {
            if (text.empty())
                return fallback;
            const std::filesystem::path path(text);
            return (path.is_absolute() ? path : sourceRoot / path).lexically_normal();
        }

        std::string CommandPath(const std::filesystem::path &path, const std::filesystem::path &sourceRoot)
        {
            const std::string text = CheckoutPath(path, sourceRoot);
            return text.find(' ') == std::string::npos ? text : "\"" + text + "\"";
        }
    }

    std::string CheckoutPath(const std::filesystem::path &path, const std::filesystem::path &sourceRoot)
    {
        const std::filesystem::path relative = path.lexically_relative(sourceRoot);
        const bool inside = !relative.empty() && *relative.begin() != "..";
        return (inside ? relative : path).generic_string();
    }

    std::string_view BuildProfileName(BuildProfile profile)
    {
        return profile == BuildProfile::Development ? "development" : "release";
    }

    std::string_view PlayerPreset(BuildProfile profile)
    {
        return profile == BuildProfile::Development ? "windows-msvc-game-debug" : "windows-msvc-game-release";
    }

    BuildProfile ReadBuildProfile(const std::filesystem::path &projectSettings)
    {
        const std::optional<json> root = ReadJsonFile(projectSettings);
        if (!root || !root->is_object())
            return BuildProfile::Release;

        const auto build = root->find("build");
        if (build == root->end() || !build->is_object())
            return BuildProfile::Release;
        const auto profile = build->find("profile");
        if (profile != build->end() && profile->is_string() &&
            profile->get<std::string>() == BuildProfileName(BuildProfile::Development))
            return BuildProfile::Development;
        return BuildProfile::Release;
    }

    bool WriteBuildProfile(const std::filesystem::path &projectSettings, BuildProfile profile)
    {
        return UpdateJsonFile(projectSettings, [profile](json &root) {
            json &build = root["build"];
            if (!build.is_object())
                build = json::object();
            build["profile"] = BuildProfileName(profile);
        });
    }

    std::filesystem::path DefaultPlayerTemplate(const std::filesystem::path &sourceRoot, BuildProfile profile)
    {
        return sourceRoot / "builds" / "player" / (profile == BuildProfile::Development ? "debug" : "release");
    }

    std::filesystem::path DefaultBuildOutput(const BuildGame &game)
    {
        return game.mSourceRoot / "builds" / "player-out" / game.mGameDir.filename();
    }

    std::string BuildOutputKey(const BuildGame &game) { return game.mGameDir.lexically_normal().generic_string(); }

    std::filesystem::path PlayerTemplateDir(
        const EditorPreferences &preferences, const BuildGame &game, BuildProfile profile)
    {
        const std::string &text = profile == BuildProfile::Development ? preferences.buildDevelopmentTemplate
                                                                       : preferences.buildReleaseTemplate;
        return FromCheckout(text, game.mSourceRoot, DefaultPlayerTemplate(game.mSourceRoot, profile));
    }

    std::filesystem::path BuildOutputDir(const EditorPreferences &preferences, const BuildGame &game)
    {
        const auto it = preferences.buildOutputs.find(BuildOutputKey(game));
        return FromCheckout(
            it != preferences.buildOutputs.end() ? it->second : std::string(), game.mSourceRoot, DefaultBuildOutput(game));
    }

    std::vector<std::string> PlayerInstallCommands(
        const std::filesystem::path &sourceRoot, const std::filesystem::path &templateDir, BuildProfile profile)
    {
        const std::string preset(PlayerPreset(profile));
        return {
            "cmake --preset " + preset,
            "cmake --build --preset " + preset,
            "cmake --install builds/" + preset + " --prefix " + CommandPath(templateDir, sourceRoot),
        };
    }

    PlayerTemplateCheck CheckPlayerTemplate(
        const std::filesystem::path &templateDir, BuildProfile profile, const std::filesystem::path &editorInfo)
    {
        PlayerTemplateCheck check;
        if (std::optional<std::string> problem = PlayerTemplateProblem(templateDir, &check.mInfo))
        {
            check.mProblem = std::move(*problem);
            return check;
        }

        const std::string expected = profile == BuildProfile::Development ? "Debug" : "Release";
        if (const auto config = check.mInfo.find("config");
            config != check.mInfo.end() && config->is_string() && config->get<std::string>() != expected)
            check.mWarnings.push_back("the player template is a " + config->get<std::string>() + " build, but the " +
                (profile == BuildProfile::Development ? "Development" : "Release") + " profile packages a " + expected +
                " one");

        const std::optional<json> editor = ReadJsonFile(editorInfo);
        if (!editor || !editor->is_object())
        {
            check.mWarnings.push_back("could not read the editor's " + editorInfo.string() +
                ", so the player template was not compared with it");
            return check;
        }
        for (const PlayerInfoMismatch &mismatch : ComparePlayerInfo(check.mInfo, *editor))
            check.mWarnings.push_back(mismatch.mMessage);
        return check;
    }
}
