/**
 * @file GameUIModule.h
 * @author sumin.park
 * @brief The game UI's context and the screens drawn into it each frame.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "UIContext.h"
#include "UIState.h"

#include <core/ecs/ModuleContext.h>
#include <core/RegistrationList.h>

#include <functional>

namespace mir
{
    // what draws the frame is a backend bridge (gameui_imgui), which calls RunFrame
    class GameUIModule
    {
    public:
        explicit GameUIModule(ModuleContext context);

        GameUIModule(const GameUIModule &) = delete;
        GameUIModule &operator=(const GameUIModule &) = delete;

        UIContext &Context() { return mUI; }

        // drawn in registration order each frame; dt is the frame's
        using Screen = std::function<void(UIContext &ui, float dt)>;
        [[nodiscard]] Registration AddScreen(Screen screen);

        void RunFrame(const UIRect &area, const UIPointer &pointer);
        void SetSuspended(bool suspended) { mSuspended = suspended; }
        bool Suspended() const { return mSuspended; }

    private:
        World &mWorld;
        UIContext mUI;
        RegistrationList<Screen> mScreens;
        float mFrameDt = 0.0f;
        bool mSuspended = false;

        // last: unregisters before anything above is destroyed
        Registration mFrameBegin;
    };
}
