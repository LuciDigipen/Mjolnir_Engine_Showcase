/**
 * @file EditorPreferences.cpp
 * @author Jackey Cen
 * @brief Per-user editor preferences, stored outside any project.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <editor/EditorPreferences.h>

#include <core/fs/Paths.h>
#include <core/log/Log.h>
#include <serial/JsonFile.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <system_error>
#include <utility>

namespace mir
{
    namespace
    {
        using json = nlohmann::json;

        constexpr const char *kFitNames[] = {"cover", "contain", "stretch"};
        constexpr const char *kPanelsNames[] = {"theme", "black", "transparent"};
        constexpr const char *kThemeNames[] = {"midnight", "slate"};

        // leaves @p value alone unless @p key holds one of @p names
        template <typename Enum, size_t N>
        void ReadEnum(const json &object, const char *key, const char *const (&names)[N], Enum &value)
        {
            const auto it = object.find(key);
            if (it == object.end() || !it->is_string())
                return;
            const std::string text = it->get<std::string>();
            for (size_t i = 0; i < N; ++i)
            {
                if (text == names[i])
                    value = static_cast<Enum>(i);
            }
        }

        void ReadBuild(const json &root, EditorPreferences &prefs)
        {
            const auto build = root.find("build");
            if (build == root.end() || !build->is_object())
                return;

            for (const auto &[key, value] : { std::pair{ "developmentTemplate", &prefs.buildDevelopmentTemplate },
                     std::pair{ "releaseTemplate", &prefs.buildReleaseTemplate } })
            {
                if (const auto it = build->find(key); it != build->end() && it->is_string())
                    *value = it->get<std::string>();
            }
            if (const auto it = build->find("outputs"); it != build->end() && it->is_object())
            {
                for (const auto &[game, output] : it->items())
                {
                    if (output.is_string() && !output.get<std::string>().empty())
                        prefs.buildOutputs[game] = output.get<std::string>();
                }
            }
            for (const auto &[key, value] : { std::pair{ "runAfterBuild", &prefs.buildRunAfter },
                     std::pair{ "compileGameBuild", &prefs.buildCompileGameBuild } })
            {
                if (const auto it = build->find(key); it != build->end() && it->is_boolean())
                    *value = it->get<bool>();
            }
        }
    }

    EditorPreferences EditorPreferences::LoadFile(const std::filesystem::path &path)
    {
        EditorPreferences prefs;

        std::error_code ec;
        if (!std::filesystem::exists(path, ec))
            return prefs;

        const std::optional<json> root = ReadJsonFile(path);
        if (!root || !root->is_object())
        {
            MIR_LOG_WARN("Editor preferences '{}' are unreadable; using defaults", path.string());
            return prefs;
        }

        ReadEnum(*root, "theme", kThemeNames, prefs.theme);
        ReadBuild(*root, prefs);

        const auto background = root->find("background");
        if (background == root->end() || !background->is_object())
            return prefs;

        if (const auto it = background->find("enabled"); it != background->end() && it->is_boolean())
            prefs.backgroundEnabled = it->get<bool>();

        // only a bare file name, so the image always stays inside the preferences directory
        if (const auto it = background->find("file"); it != background->end() && it->is_string())
        {
            const std::string file = it->get<std::string>();
            if (file != "." && file != ".." && file.find_first_of("/\\:") == std::string::npos)
                prefs.backgroundFile = file;
        }

        if (const auto it = background->find("strength"); it != background->end() && it->is_number())
        {
            const float strength = it->get<float>();
            if (std::isfinite(strength))
                prefs.backgroundStrength = std::clamp(strength, 0.0f, 1.0f);
        }

        ReadEnum(*background, "fit", kFitNames, prefs.backgroundFit);
        ReadEnum(*background, "panels", kPanelsNames, prefs.backgroundPanels);

        return prefs;
    }

    bool EditorPreferences::SaveFile(const std::filesystem::path &path) const
    {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec)
        {
            MIR_LOG_ERROR("Cannot create '{}': {}", path.parent_path().string(), ec.message());
            return false;
        }

        const json root{{"theme", kThemeNames[static_cast<size_t>(theme)]},
                        {"background",
                         {{"enabled", backgroundEnabled},
                          {"file", backgroundFile},
                          {"strength", backgroundStrength},
                          {"fit", kFitNames[static_cast<size_t>(backgroundFit)]},
                          {"panels", kPanelsNames[static_cast<size_t>(backgroundPanels)]}}},
                        {"build",
                         {{"developmentTemplate", buildDevelopmentTemplate},
                          {"releaseTemplate", buildReleaseTemplate},
                          {"outputs", buildOutputs},
                          {"runAfterBuild", buildRunAfter},
                          {"compileGameBuild", buildCompileGameBuild}}}};
        return WriteJsonFile(path, root);
    }

    std::filesystem::path EditorPreferencesDir() { return UserDataDir() / "editor"; }

    std::filesystem::path EditorPreferencesPath() { return EditorPreferencesDir() / "editor_preferences.json"; }
}
