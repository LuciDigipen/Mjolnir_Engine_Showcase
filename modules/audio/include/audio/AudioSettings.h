/**
 * @file AudioSettings.h
 * @author Quy Dong Ta
 * @brief The music and SFX volume and mute, kept in a per-user file between runs.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace mir
{
    struct ChannelSettings
    {
        float mVolume = 1.0f;
        bool mMuted = false;

        bool operator==(const ChannelSettings &) const = default;
    };

    struct AudioSettings
    {
        ChannelSettings mMusic;
        ChannelSettings mSfx;

        bool operator==(const AudioSettings &) const = default;
    };

    // A missing or unreadable file, or a bad value, falls back to defaults and is logged here
    AudioSettings LoadAudioSettings(const std::filesystem::path &path, const AudioSettings &defaults = {});

    // Writes the "audio" section and keeps any other section the file holds. False when it cannot + loggs it
    bool SaveAudioSettings(const std::filesystem::path &path, const AudioSettings &settings);

    // The "audio" section of a project settings file, for a panel that shows why a reload failed. Unlike
    // LoadAudioSettings it returns nullopt and sets problem when the file is missing, is not a JSON object or holds
    // a bad value, and it logs nothing. A file with no "audio" section gives the built-in defaults.
    std::optional<AudioSettings> ReadAudioDefaults(const std::filesystem::path &path, std::string &problem);

    AudioSettings CurrentAudioSettings();

    void ApplyAudioSettings(const AudioSettings &settings);

    bool ApplyChangedDefaults(const AudioSettings &defaults, const std::filesystem::path &playerFile);
}
