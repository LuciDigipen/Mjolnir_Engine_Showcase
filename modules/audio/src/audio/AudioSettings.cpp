/**
 * @file AudioSettings.cpp
 * @author Quy Dong Ta
 * @brief The music and SFX volume and mute, kept in a per-user file between runs.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <audio/AudioSettings.h>

#include <audio/AudioSystem.h>

#include <core/fs/FileIO.h>
#include <core/log/Log.h>
#include <serial/JsonFile.h>

#include <nlohmann/json.hpp>

#include <cmath>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace mir
{
    namespace
    {
        using json = nlohmann::json;

        constexpr const char *kSection = "audio";

       
        std::optional<json> ReadObject(const std::filesystem::path &path)
        {
            std::error_code ec;
            if (!std::filesystem::is_regular_file(path, ec))
                return std::nullopt;

            const std::optional<std::string> text = ReadText(path);
            if (!text)
                return std::nullopt;

            json root = json::parse(*text, nullptr, false);
            if (root.is_discarded() || !root.is_object())
                return std::nullopt;
            return root;
        }

        void ReadChannel(const json &audio, const char *name, ChannelSettings &out, std::vector<std::string> &problems)
        {
            const auto channel = audio.find(name);
            if (channel == audio.end())
                return;

            if (!channel->is_object())
            {
                problems.push_back(std::string(name) + " is not an object");
                return;
            }

            if (const auto volume = channel->find("volume"); volume != channel->end())
            {
                const bool valid = volume->is_number() && volume->get<double>() >= 0.0 && volume->get<double>() <= 1.0;
                if (valid)
                    out.mVolume = volume->get<float>();
                else
                    problems.push_back(std::string(name) + ".volume is not a number from 0 to 1");
            }

            if (const auto muted = channel->find("muted"); muted != channel->end())
            {
                if (muted->is_boolean())
                    out.mMuted = muted->get<bool>();
                else
                    problems.push_back(std::string(name) + ".muted is not true or false");
            }
        }

        std::string Describe(const ChannelSettings &channel)
        {
            const int percent = static_cast<int>(std::lround(channel.mVolume * 100.0f));
            return std::to_string(percent) + "%" + (channel.mMuted ? " (muted)" : "");
        }

        json WriteChannel(const ChannelSettings &channel)
        {
            // 0.3f would otherwise be written as 0.30000001192092896
            return {{"volume", std::round(static_cast<double>(channel.mVolume) * 1000.0) / 1000.0},
                    {"muted", channel.mMuted}};
        }
    }

    AudioSettings LoadAudioSettings(const std::filesystem::path &path, const AudioSettings &defaults)
    {
        AudioSettings settings = defaults;

        std::error_code ec;
        if (!std::filesystem::exists(path, ec))
        {
            MIR_LOG_INFO("no audio settings at '{}' yet; using the defaults", path.string());
            return settings;
        }

        const std::optional<json> root = ReadObject(path);
        if (!root)
        {
            MIR_LOG_WARN("audio settings at '{}' are not a readable JSON object; using the defaults", path.string());
            return settings;
        }

        const auto audio = root->find(kSection);
        if (audio == root->end())
            return settings;

        std::vector<std::string> problems;
        if (audio->is_object())
        {
            ReadChannel(*audio, "music", settings.mMusic, problems);
            ReadChannel(*audio, "sfx", settings.mSfx, problems);
        }
        else
            problems.push_back("audio is not an object");

        if (!problems.empty())
        {
            std::string list;
            for (const std::string &problem : problems)
                list += (list.empty() ? "" : "; ") + problem;
            MIR_LOG_WARN("audio settings at '{}': {}; the default is used instead", path.string(), list);
        }

        MIR_LOG_INFO("audio settings from '{}': music {}, SFX {}", path.string(), Describe(settings.mMusic),
            Describe(settings.mSfx));
        return settings;
    }

    bool SaveAudioSettings(const std::filesystem::path &path, const AudioSettings &settings)
    {
        json root = ReadObject(path).value_or(json::object());
        root[kSection] = {{"music", WriteChannel(settings.mMusic)}, {"sfx", WriteChannel(settings.mSfx)}};

        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec)
        {
            MIR_LOG_ERROR("audio settings: cannot create '{}' ({})", path.parent_path().string(), ec.message());
            return false;
        }

        return WriteJsonFile(path, root);
    }

    AudioSettings CurrentAudioSettings()
    {
        return {{audio::GetMusicVolume(), audio::IsMusicMuted()}, {audio::GetSfxVolume(), audio::IsSfxMuted()}};
    }

    void ApplyAudioSettings(const AudioSettings &settings)
    {
        audio::SetMusicVolume(settings.mMusic.mVolume);
        audio::SetMusicMuted(settings.mMusic.mMuted);
        audio::SetSfxVolume(settings.mSfx.mVolume);
        audio::SetSfxMuted(settings.mSfx.mMuted);
    }

    bool ApplyChangedDefaults(const AudioSettings &defaults, const std::filesystem::path &playerFile)
    {
        std::error_code ec;
        if (std::filesystem::exists(playerFile, ec))
            return false;

        ApplyAudioSettings(defaults);
        return true;
    }
}
