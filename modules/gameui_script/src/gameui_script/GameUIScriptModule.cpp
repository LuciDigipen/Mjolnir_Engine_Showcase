/**
 * @file GameUIScriptModule.cpp
 * @author Quy Dong Ta
 * @brief Draws the game UI that scripts author: every script's OnUI, as one Game screen.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <gameui_script/GameUIScriptModule.h>

#include <gameui_script/UIBindings.h>

#include <assets/AssetsModule.h>
#include <core/ecs/World.h>
#include <gameui/GameUIModule.h>
#include <script/BindingApi.h>
#include <script/ScriptModule.h>

namespace mir
{
    GameUIScriptModule::GameUIScriptModule(ModuleContext context, ScriptModule &scripts, GameUIModule &gameUI,
        AssetsModule &assets, const AssetResolver &resolver)
        : mWorld(context.world), mHost(scripts.Host()), mFrame(std::make_shared<UIScriptFrame>())
    {
        mFrame->mAssets = &assets.Cache();
        mFrame->mResolver = &resolver;
        mBindings = AddBindings(mHost, &RegisterUIBindings);
        mScreen = gameUI.AddScreen(UIScreenLayer::Game,
            [this](UIContext &ui, float dt) { DrawScriptScreens(mHost, mWorld, mFrame, ui, dt); });
    }

    GameUIScriptModule::~GameUIScriptModule()
    {
        mScreen.Reset();
        mBindings.Reset();
    }
}
