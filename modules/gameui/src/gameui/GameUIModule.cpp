/**
 * @file GameUIModule.cpp
 * @author sumin.park
 * @brief The game UI's context and the screens drawn into it each frame.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <gameui/GameUIModule.h>

#include <gameui/GameUIArea.h>
#include <gameui/GameUIDemo.h>

#include <core/ecs/SystemScheduler.h>
#include <core/ecs/World.h>
#include <input/InputMap.h>
#include <input/InputState.h>

#include <utility>

namespace mir
{
    GameUIModule::GameUIModule(ModuleContext context) : mWorld(context.world)
    {
        context.world.EmplaceResource<GameUIArea>();
        context.world.EmplaceResource<GameUIDemo>();

        mFrameBegin = context.scheduler.AddFrameSlot(FrameSlot::FrameBegin,
            [this](SystemContext &frame) { mFrameDt = frame.dt; });
    }

    Registration GameUIModule::AddScreen(Screen screen) { return mScreens.Add(std::move(screen)); }

    void GameUIModule::RunFrame(const UIRect &area, const UIPointer &pointer)
    {
        if (mSuspended)
            return;
        mUI.BeginFrame(area, mWorld.Resource<InputState>(), mWorld.Resource<InputMap>(), pointer);
        mScreens.Run(mUI, mFrameDt);
        mUI.EndFrame();
    }
}
