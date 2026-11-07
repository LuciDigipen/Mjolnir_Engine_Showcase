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

#include <project/GameBuildInfo.h>
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

    std::string_view GameBuildPreset(BuildProfile profile)
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

    bool WriteGameBuildChoice(const std::filesystem::path &projectSettings, std::string_view key, std::optional<bool> on)
    {
        return UpdateJsonFile(projectSettings, [key, on](json &root) {
            const std::string options(kGameBuildOptionsKey);
            if (!on)
            {
                // nothing to remove, and no empty build section to add
                const auto build = root.find("build");
                if (build == root.end() || !build->is_object() || !build->contains(options))
                    return;
                json &chosen = (*build)[options];
                if (chosen.is_object())
                    chosen.erase(std::string(key));
                if (!chosen.is_object() || chosen.empty())
                    build->erase(options);
                return;
            }

            json &build = root["build"];
            if (!build.is_object())
                build = json::object();
            json &chosen = build[options];
            if (!chosen.is_object())
                chosen = json::object();
            chosen[std::string(key)] = *on;
        });
    }

    std::vector<std::string> ChangedGameBuildOptions(
        const json &templateInfo, const GameBuildChoices &choices, const json &editor)
    {
        std::vector<std::string> changed;
        for (const GameBuildFeature &feature : kGameBuildFeatures)
        {
            const std::optional<bool> built = GameBuildInfoFlag(templateInfo, feature.mKey);
            const std::optional<bool> wanted = WantedGameBuildFlag(feature, choices, editor);
            if (built && wanted && *built != *wanted)
                changed.emplace_back(feature.mLabel);
        }
        return changed;
    }

    std::filesystem::path DefaultGameBuildTemplate(const std::filesystem::path &sourceRoot, BuildProfile profile)
    {
        return sourceRoot / "builds" / "game-build" / (profile == BuildProfile::Development ? "debug" : "release");
    }

    std::filesystem::path DefaultBuildOutput(const BuildGame &game)
    {
        return game.mSourceRoot / "builds" / "game-build-out" / game.mGameDir.filename();
    }

    std::string BuildOutputKey(const BuildGame &game) { return game.mGameDir.lexically_normal().generic_string(); }

    std::filesystem::path GameBuildTemplateDir(
        const EditorPreferences &preferences, const BuildGame &game, BuildProfile profile)
    {
        const std::string &text = profile == BuildProfile::Development ? preferences.buildDevelopmentTemplate
                                                                       : preferences.buildReleaseTemplate;
        return FromCheckout(text, game.mSourceRoot, DefaultGameBuildTemplate(game.mSourceRoot, profile));
    }

    std::filesystem::path BuildOutputDir(const EditorPreferences &preferences, const BuildGame &game)
    {
        const auto it = preferences.buildOutputs.find(BuildOutputKey(game));
        return FromCheckout(
            it != preferences.buildOutputs.end() ? it->second : std::string(), game.mSourceRoot, DefaultBuildOutput(game));
    }

    GameBuildTemplateCheck CheckGameBuildTemplate(const std::filesystem::path &templateDir, BuildProfile profile,
        const std::filesystem::path &editorInfo, const GameBuildChoices &choices)
    {
        GameBuildTemplateCheck check;
        if (std::optional<std::string> problem = GameBuildTemplateProblem(templateDir, &check.mInfo))
        {
            check.mProblem = std::move(*problem);
            return check;
        }

        const std::string expected = profile == BuildProfile::Development ? "Debug" : "Release";
        if (const auto config = check.mInfo.find("config");
            config != check.mInfo.end() && config->is_string() && config->get<std::string>() != expected)
            check.mWarnings.push_back("the game build template is a " + config->get<std::string>() + " build, but the " +
                (profile == BuildProfile::Development ? "Development" : "Release") + " profile packages a " + expected +
                " one");

        // the choices are compared even without the editor's
        std::optional<json> editor = ReadJsonFile(editorInfo);
        if (!editor || !editor->is_object())
        {
            check.mWarnings.push_back("could not read the editor's " + editorInfo.string() +
                ", so the game build template was not compared with it");
            editor = json();
        }
        for (const GameBuildInfoMismatch &mismatch : CompareGameBuildInfo(check.mInfo, *editor, choices))
            check.mWarnings.push_back(mismatch.mMessage);
        return check;
    }
}
