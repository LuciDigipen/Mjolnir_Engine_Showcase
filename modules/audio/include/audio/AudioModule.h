/**
 * @file AudioModule.h
 * @author sumin.park
 * @brief Wwise, audio components and the systems that drive them.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/ecs/ModuleContext.h>
#include <core/Registration.h>

#include <filesystem>

namespace mir
{
    class AudioModule
    {
    public:
        // Restores the volumes from settingsPath, starts Wwise and queues every bank in
        // bankDir also throws SetupError when it cannot start
        AudioModule(ModuleContext context, const std::filesystem::path &bankDir,
            const std::filesystem::path &settingsPath);
        ~AudioModule();

        AudioModule(const AudioModule &) = delete;
        AudioModule &operator=(const AudioModule &) = delete;

    private:
        // Wwise is process state, not a member, so the destructor drops this before Terminate
        Registration mHooks;
        Registration mSoundtrackHooks;
        Registration mPlayEnd;
    };
}
