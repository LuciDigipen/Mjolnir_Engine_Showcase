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

    // A missing or unreadable file, or a bad value, falls back to the default and is logged here
    AudioSettings LoadAudioSettings(const std::filesystem::path &path);

    // Writes the "audio" section and keeps any other section the file holds. False when it cannot + loggs it
    bool SaveAudioSettings(const std::filesystem::path &path, const AudioSettings &settings);

    AudioSettings CurrentAudioSettings();

    void ApplyAudioSettings(const AudioSettings &settings);
}
