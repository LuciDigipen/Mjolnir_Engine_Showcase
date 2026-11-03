/**
 * @file AudioModule.h
 * @author sumin.park
 * @brief Wwise, audio components and the systems that drive them.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <audio/AudioSettings.h>

#include <core/ecs/ModuleContext.h>
#include <core/Registration.h>

#include <filesystem>
#include <memory>

namespace mir
{
    class AudioModule
    {
    public:
        // Restores the volumes from settingsPath, or the project's defaults when it has none yet
        // starts Wwise and queues every bank in bankDir also throws SetupError when it cannot start
        AudioModule(ModuleContext context, const std::filesystem::path &bankDir,
            const std::filesystem::path &projectSettingsPath, const std::filesystem::path &settingsPath);
        ~AudioModule();

        AudioModule(const AudioModule &) = delete;
        AudioModule &operator=(const AudioModule &) = delete;

        // The "audio" section of project_settings.json, as read at startup or last saved
        const AudioSettings &Defaults() const { return mDefaults; }

        // Writes the "audio" section of project_settings.json and keeps it as Defaults(); false when it cannot
        bool SaveDefaults(const AudioSettings &defaults);

        class WorldBinding;
        std::unique_ptr<WorldBinding> BindWorld(ModuleContext context);

        class WorldBinding
        {
        public:
            explicit WorldBinding(ModuleContext context);
            ~WorldBinding();
            WorldBinding(const WorldBinding &) = delete;
            WorldBinding &operator=(const WorldBinding &) = delete;

        private:
            Registration mHooks;
            Registration mSoundtrackHooks;
            Registration mPlayEnd;
        };

    private:
        std::filesystem::path mProjectSettingsPath;
        AudioSettings mDefaults;

        // Wwise is process state, not a member, so the destructor drops this before Terminate
        Registration mHooks;
        Registration mSoundtrackHooks;
        Registration mPlayEnd;
    };
}
