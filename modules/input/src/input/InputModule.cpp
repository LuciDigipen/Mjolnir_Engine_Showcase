/**
 * @file InputModule.cpp
 * @author sumin.park
 * @brief The input map, input state, the player's look options and the system that reads the window.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <input/InputModule.h>

#include <input/InputState.h>
#include <input/InputSystem.h>

#include <core/ecs/SystemScheduler.h>
#include <core/ecs/World.h>

namespace mir
{
    InputModule::InputModule(ModuleContext context, Window &window, const std::filesystem::path &userSettingsPath,
        ControlsSettings *sharedControls)
        : mWorld(context.world), mUserSettingsPath(userSettingsPath)
    {
        context.world.EmplaceResource<InputState>();
        if (sharedControls)
            context.world.ShareResource(*sharedControls);
        else
            context.world.EmplaceResource<ControlsSettings>(
                mUserSettingsPath.empty() ? ControlsSettings{} : LoadControlsSettings(mUserSettingsPath));

        // before ScriptSystem, which reads this frame's input
        context.scheduler.AddSystem<InputSystem>(SystemPhase::PreUpdate, SystemOrder{SystemOrder::kEarly}, window);
    }

    const ControlsSettings &InputModule::Controls() const
    {
        return mWorld.Resource<ControlsSettings>();
    }

    void InputModule::SetControls(const ControlsSettings &controls)
    {
        ControlsSettings &current = mWorld.Resource<ControlsSettings>();
        if (current == controls)
            return;

        current = controls;
        if (!mUserSettingsPath.empty())
            SaveControlsSettings(mUserSettingsPath, controls);
    }
}
