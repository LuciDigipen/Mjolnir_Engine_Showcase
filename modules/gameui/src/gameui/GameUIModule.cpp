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

#include <core/ecs/PrimaryView.h>
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
        context.world.EmplaceResource<GameUIPointer>();
        context.world.EmplaceResource<GameUIDemo>();

        mFrameBegin = context.scheduler.AddFrameSlot(FrameSlot::FrameBegin,
            [this](SystemContext &frame) { mFrameDt = frame.dt; });
    }

    Registration GameUIModule::AddScreen(UIScreenLayer layer, Screen screen)
    {
        return mScreens[static_cast<std::size_t>(layer)].Add(std::move(screen));
    }

    void GameUIModule::RunFrame(const UIRect &area, const UIPointer &pointer)
    {
        GameUIPointer &owned = mWorld.Resource<GameUIPointer>();
        owned.mOverWidget = false;
        if (mSuspended)
        {
            mShellHeldInput = false;
            return;
        }
        // the renderer's, written last in LateUpdate, a world without a renderer has none
        const PrimaryView *view = mWorld.TryResource<PrimaryView>();
        mUI.SetView(view != nullptr ? *view : PrimaryView{});
        mUI.BeginFrame(area, mWorld.Resource<InputState>(), mWorld.Resource<InputMap>(), pointer);
        mUI.SetWidgetsDisabled(mShellHeldInput);
        mScreens[static_cast<std::size_t>(UIScreenLayer::Game)].Run(mUI, mFrameDt);
        mUI.SetWidgetsDisabled(false);
        // a style a Game screen left pushed never reaches the shell
        while (mUI.StyleDepth() > 0)
            mUI.PopStyle();
        const uint32_t heldBefore = mUI.HoldInputCalls();
        mScreens[static_cast<std::size_t>(UIScreenLayer::Shell)].Run(mUI, mFrameDt);
        mShellHeldInput = mUI.HoldInputCalls() != heldBefore;
        mUI.EndFrame();
        owned.mOverWidget = !mUI.State().Hot().IsNull() || !mUI.State().Active().IsNull();
    }
}
