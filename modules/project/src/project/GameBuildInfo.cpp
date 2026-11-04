/**
 * @file GameBuildInfo.cpp
 * @author Quy Dong Ta
 * @brief game_build_info.json, which says how an engine build was made: checking a game build
 *        template and comparing it with the editor.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <project/GameBuildInfo.h>

#include <serial/JsonFile.h>

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

    std::vector<GameBuildInfoMismatch> CompareGameBuildInfo(const nlohmann::json &gameBuild, const nlohmann::json &editor)
    {
        std::vector<GameBuildInfoMismatch> mismatches;
        if (GameBuildInfoVersion(gameBuild) != GameBuildInfoVersion(editor))
            mismatches.push_back({ "version", "the game build template is engine " + GameBuildInfoVersion(gameBuild) +
                ", the editor " + GameBuildInfoVersion(editor) + "; build the template and the editor from the same commit" });

        for (const GameBuildFeature &feature : kGameBuildFeatures)
        {
            const std::optional<bool> ours = GameBuildInfoFlag(gameBuild, feature.mKey);
            const std::optional<bool> theirs = GameBuildInfoFlag(editor, feature.mKey);
            if (!ours || !theirs || *ours == *theirs)
                continue;
            mismatches.push_back({ std::string(feature.mKey), "the game build template was built with " +
                std::string(feature.mOption) + (*ours ? " ON" : " OFF") + " and the editor with it " +
                (*theirs ? "ON" : "OFF") });
        }
        return mismatches;
    }
}
