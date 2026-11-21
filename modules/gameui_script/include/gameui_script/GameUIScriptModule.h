/**
 * @file GameUIScriptModule.h
 * @author Quy Dong Ta
 * @brief Draws the game UI that scripts author: every script's OnUI, as one Game screen.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/ecs/ModuleContext.h>
#include <core/Registration.h>

#include <memory>

namespace mir
{
    class AssetResolver;
    class AssetsModule;
    class GameUIModule;
    class ScriptHost;
    class ScriptModule;
    struct UIScriptFrame;

    // A bridge: gameui never sees Lua, and script never sees gameui. Declared after ScriptModule and the world's
    // GameUIModule
    class GameUIScriptModule
    {
    public:
        GameUIScriptModule(ModuleContext context, ScriptModule &scripts, GameUIModule &gameUI, AssetsModule &assets,
            const AssetResolver &resolver);
        ~GameUIScriptModule();

        GameUIScriptModule(const GameUIScriptModule &) = delete;
        GameUIScriptModule &operator=(const GameUIScriptModule &) = delete;

    private:
        World &mWorld;
        ScriptHost &mHost;
        std::shared_ptr<UIScriptFrame> mFrame;

        // last, unregister before anything above is destroyed
        Registration mBindings;
        Registration mScreen;
    };
}
