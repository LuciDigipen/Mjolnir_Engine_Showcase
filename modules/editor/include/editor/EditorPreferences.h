/**
 * @file EditorPreferences.h
 * @author Jackey Cen
 * @brief Per-user editor preferences, stored outside any project.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <filesystem>
#include <map>
#include <string>

namespace mir
{
    enum class BackgroundFit
    {
        Cover,
        Contain,
        Stretch,
    };

    // mirrors EditorThemeKind; editortheme is private to editor
    enum class EditorThemeChoice
    {
        Midnight,
        Slate,
    };

    // what the dock panels are filled with while the image shows
    enum class BackgroundPanels
    {
        Theme,
        Black,
        Transparent,
    };

    struct EditorPreferences
    {
        EditorThemeChoice theme = EditorThemeChoice::Midnight;

        bool backgroundEnabled = false;
        // file name inside EditorPreferencesDir(); empty is no image
        std::string backgroundFile;
        // how much of the image shows through the panels, 0..1; unused with Transparent panels
        float backgroundStrength = 0.2f;
        BackgroundFit backgroundFit = BackgroundFit::Cover;
        BackgroundPanels backgroundPanels = BackgroundPanels::Theme;

        // File > Build Settings, kept here because paths differ per machine. Empty is the
        // default under the checkout's builds/, and a relative path is from the checkout
        std::string buildDevelopmentTemplate;
        std::string buildReleaseTemplate;
        // keyed by the game folder's absolute path, so each game builds into its own folder
        std::map<std::string, std::string> buildOutputs;
        bool buildRunAfter = false;
        // Build compiles the game build into the template first, off uses the template as it is
        bool buildCompileGameBuild = true;

        // defaults for a missing file and for any missing or unusable field
        static EditorPreferences LoadFile(const std::filesystem::path &path);
        bool SaveFile(const std::filesystem::path &path) const;
    };

    // %LOCALAPPDATA%/Mjolnir/editor on Windows, next to the executable elsewhere
    std::filesystem::path EditorPreferencesDir();
    std::filesystem::path EditorPreferencesPath();
}
