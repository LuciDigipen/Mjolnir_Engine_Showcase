/**
 * @file PlayerInfo.cpp
 * @author Quy Dong Ta
 * @brief player_info.json, which says how an engine build was made: checking a player
 *        template and comparing it with the editor.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <project/PlayerInfo.h>

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

    std::optional<bool> PlayerInfoFlag(const nlohmann::json &info, std::string_view key)
    {
        const auto it = info.find(std::string(key));
        if (it == info.end() || !it->is_boolean())
            return std::nullopt;
        return it->get<bool>();
    }

    std::string PlayerInfoVersion(const nlohmann::json &info)
    {
        return Text(info, "engineVersion") + " at commit " + Text(info, "commit").substr(0, 10);
    }

    std::optional<std::string> PlayerTemplateProblem(const std::filesystem::path &dir, nlohmann::json *info)
    {
        std::error_code ec;
        if (!std::filesystem::is_directory(dir, ec))
            return "the player template " + PathText(dir) + " does not exist";
        if (!std::filesystem::is_regular_file(dir / kPlayerExecutableFileName, ec))
            return PathText(dir) + " has no " + std::string(kPlayerExecutableFileName);

        const std::optional<nlohmann::json> read = ReadJsonFile(dir / kPlayerInfoFileName);
        if (!read || !read->is_object())
            return PathText(dir) + " has no readable " + std::string(kPlayerInfoFileName) +
                ", so it is not a player template";
        if (read->value("editor", true))
            return PathText(dir) + " is an editor build, not a player";

        if (info != nullptr)
            *info = *read;
        return std::nullopt;
    }

    std::vector<PlayerInfoMismatch> ComparePlayerInfo(const nlohmann::json &player, const nlohmann::json &editor)
    {
        std::vector<PlayerInfoMismatch> mismatches;
        if (PlayerInfoVersion(player) != PlayerInfoVersion(editor))
            mismatches.push_back({ "version", "the player template is engine " + PlayerInfoVersion(player) +
                ", the editor " + PlayerInfoVersion(editor) + "; build the template and the editor from the same commit" });

        for (const PlayerFeature &feature : kPlayerFeatures)
        {
            const std::optional<bool> ours = PlayerInfoFlag(player, feature.mKey);
            const std::optional<bool> theirs = PlayerInfoFlag(editor, feature.mKey);
            if (!ours || !theirs || *ours == *theirs)
                continue;
            mismatches.push_back({ std::string(feature.mKey), "the player template was built with " +
                std::string(feature.mOption) + (*ours ? " ON" : " OFF") + " and the editor with it " +
                (*theirs ? "ON" : "OFF") });
        }
        return mismatches;
    }
}
