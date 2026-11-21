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

#include <cstddef>
#include <cstdint>
#include <functional>

namespace mir
{
    // Game screens are what a game draws in play, Shell screens are the engine's, such as the pause menu, and draw
    // over them
    enum class UIScreenLayer : uint8_t
    {
        Game,
        Shell,
        kCount
    };

    // what draws the frame is a backend bridge (gameui_imgui), which calls RunFrame
    class GameUIModule
    {
    public:
        explicit GameUIModule(ModuleContext context);

        GameUIModule(const GameUIModule &) = delete;
        GameUIModule &operator=(const GameUIModule &) = delete;

        UIContext &Context() { return mUI; }

        // drawn by layer, then in registration order
        using Screen = std::function<void(UIContext &ui, float dt)>;
        [[nodiscard]] Registration AddScreen(UIScreenLayer layer, Screen screen);

        // a Shell screen held the input last frame, such as an open pause menu, so this frame's Game screens draw
        // their widgets disabled under it
        bool ShellHeldInput() const { return mShellHeldInput; }

        void RunFrame(const UIRect &area, const UIPointer &pointer);
        void SetSuspended(bool suspended) { mSuspended = suspended; }
        bool Suspended() const { return mSuspended; }

    private:
        World &mWorld;
        UIContext mUI;
        RegistrationList<Screen> mScreens[static_cast<std::size_t>(UIScreenLayer::kCount)];
        float mFrameDt = 0.0f;
        bool mSuspended = false;
        bool mShellHeldInput = false;

        // last: unregisters before anything above is destroyed
        Registration mFrameBegin;
    };
}
