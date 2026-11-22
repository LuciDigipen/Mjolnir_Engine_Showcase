/**
 * @file UIBindings.h
 * @author Quy Dong Ta
 * @brief The ui object a script's OnUI is given, and the call that runs every script's OnUI in a game UI frame.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <gameui/UIContext.h>

#include <core/ecs/Entity.h>

#include <sol/forward.hpp>

#include <memory>
#include <string>
#include <unordered_map>

namespace mir
{
    class AssetCache;
    class AssetResolver;
    class ScriptHost;
    class World;

    // What OnUI's ui wraps. The context is set only while an OnUI runs, so a ui a script kept errors when used
    struct UIScriptFrame
    {
        UIContext *mUI = nullptr;
        Entity mOwner;
        AssetCache *mAssets = nullptr;
        const AssetResolver *mResolver = nullptr;
        // by path and slice, a failed one as an invalid sprite, so it is resolved and reported once
        std::unordered_map<std::string, UISprite> mSprites;
        // by path, a failed one as the default font
        std::unordered_map<std::string, UIFont> mFonts;
    };

    void RegisterUIBindings(sol::state &lua);

    // In Play, paused or not, calls OnUI(self, world, entity, ui, dt) on every script in world whose OnStart has
    // run. A failed OnUI is
    // logged and counted like any script failure, and whatever clip, opacity, transform or style it left pushed is
    // popped
    void DrawScriptScreens(ScriptHost &host, World &world, const std::shared_ptr<UIScriptFrame> &frame,
        UIContext &ui, float dt);
}
