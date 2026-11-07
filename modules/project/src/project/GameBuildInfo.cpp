/**
 * @file GameBuildInfo.cpp
 * @author Quy Dong Ta
 * @brief game_build_info.json, which says how an engine build was made: checking a game build
 *        template and comparing it with the editor and the options the game chose.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <project/GameBuildInfo.h>

#include <serial/JsonFile.h>

#include <algorithm>
#include <system_error>

namespace mir
{
    namespace
    {
        std::string PathText(const std::filesystem::path &path)
        {
            const std::u8string text = path.u8string();
            return { reinterpret_cast<const char *>(text.data()), text.size() };
        }

        std::string Text(const nlohmann::json &info, std::string_view key)
        {
            const auto it = info.find(std::string(key));
            return it != info.end() && it->is_string() ? it->get<std::string>() : "unknown";
        }
    }

    std::optional<bool> GameBuildInfoFlag(const nlohmann::json &info, std::string_view key)
    {
        const auto it = info.find(std::string(key));
        if (it == info.end() || !it->is_boolean())
            return std::nullopt;
        return it->get<bool>();
    }

    std::string GameBuildInfoVersion(const nlohmann::json &info)
    {
        return Text(info, "engineVersion") + " at commit " + Text(info, "commit").substr(0, 10);
    }

    std::optional<std::string> GameBuildTemplateProblem(const std::filesystem::path &dir, nlohmann::json *info)
    {
        std::error_code ec;
        if (!std::filesystem::is_directory(dir, ec))
            return "the game build template " + PathText(dir) + " does not exist";
        if (!std::filesystem::is_regular_file(dir / kGameBuildExecutableFileName, ec))
            return PathText(dir) + " has no " + std::string(kGameBuildExecutableFileName);

        const std::optional<nlohmann::json> read = ReadJsonFile(dir / kGameBuildInfoFileName);
        if (!read || !read->is_object())
            return PathText(dir) + " has no readable " + std::string(kGameBuildInfoFileName) +
                ", so it is not a game build template";
        if (read->value("editor", true))
            return PathText(dir) + " is an editor build, not a game build";

        if (info != nullptr)
            *info = *read;
        return std::nullopt;
    }

    GameBuildChoices ReadGameBuildChoices(const nlohmann::json &projectSettings)
    {
        GameBuildChoices choices;
        if (!projectSettings.is_object())
            return choices;
        const auto build = projectSettings.find("build");
        if (build == projectSettings.end() || !build->is_object())
            return choices;
        const auto options = build->find(std::string(kGameBuildOptionsKey));
        if (options == build->end())
            return choices;

        const std::string where = "project_settings.json's build." + std::string(kGameBuildOptionsKey);
        if (!options->is_object())
        {
            choices.mProblems.push_back(where + " is not an object, so every build option follows the editor");
            return choices;
        }

        std::string known;
        for (const GameBuildFeature &feature : kGameBuildFeatures)
            known += (known.empty() ? "" : ", ") + std::string(feature.mKey);
        for (const auto &[key, value] : options->items())
        {
            const bool feature = std::ranges::any_of(
                kGameBuildFeatures, [&](const GameBuildFeature &each) { return each.mKey == key; });
            if (!feature)
                choices.mProblems.push_back(where + "." + key + " is not a build option (" + known + "), so it is ignored");
            else if (!value.is_boolean())
                choices.mProblems.push_back(where + "." + key + " is not true or false, so it follows the editor");
            else
                choices.mOn.emplace(key, value.get<bool>());
        }
        return choices;
    }

    GameBuildChoices ReadGameBuildChoices(const std::filesystem::path &projectSettings)
    {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(projectSettings, ec))
            return {};
        const std::optional<nlohmann::json> settings = ReadJsonFile(projectSettings);
        return settings ? ReadGameBuildChoices(*settings) : GameBuildChoices{};
    }

    std::optional<bool> WantedGameBuildFlag(
        const GameBuildFeature &feature, const GameBuildChoices &choices, const nlohmann::json &editor)
    {
        if (const auto chosen = choices.mOn.find(feature.mKey); chosen != choices.mOn.end())
            return chosen->second;
        return editor.is_object() ? GameBuildInfoFlag(editor, feature.mKey) : std::nullopt;
    }

    std::vector<GameBuildInfoMismatch> CompareGameBuildInfo(
        const nlohmann::json &gameBuild, const nlohmann::json &editor, const GameBuildChoices &choices)
    {
        std::vector<GameBuildInfoMismatch> mismatches;
        const bool withEditor = editor.is_object();
        if (withEditor && GameBuildInfoVersion(gameBuild) != GameBuildInfoVersion(editor))
            mismatches.push_back({ "version", "the game build template is engine " + GameBuildInfoVersion(gameBuild) +
                ", the editor " + GameBuildInfoVersion(editor) + "; build the template and the editor from the same commit" });

        for (const GameBuildFeature &feature : kGameBuildFeatures)
        {
            const std::optional<bool> ours = GameBuildInfoFlag(gameBuild, feature.mKey);
            if (!ours)
                continue;
            const std::string built = "the game build template was built with " + std::string(feature.mOption) +
                (*ours ? " ON" : " OFF");

            // a difference from the editor that the game chose is what it asked for
            if (const auto chosen = choices.mOn.find(feature.mKey); chosen != choices.mOn.end())
            {
                if (*ours != chosen->second)
                    mismatches.push_back({ std::string(feature.mKey),
                        built + ", but the game chooses it " + (chosen->second ? "ON" : "OFF") });
                continue;
            }

            const std::optional<bool> theirs = withEditor ? GameBuildInfoFlag(editor, feature.mKey) : std::nullopt;
            if (!theirs || *ours == *theirs)
                continue;
            mismatches.push_back(
                { std::string(feature.mKey), built + " and the editor with it " + (*theirs ? "ON" : "OFF") });
        }
        return mismatches;
    }
}
