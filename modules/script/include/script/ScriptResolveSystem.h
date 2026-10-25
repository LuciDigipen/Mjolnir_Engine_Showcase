/**
 * @file ScriptResolveSystem.h
 * @author sumin.park
 * @brief Gives every ScriptRef its script instance, every frame and in every mode.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <script/components/ScriptRef.h>

#include <core/ecs/Query.h>
#include <core/ecs/System.h>

#include <functional>
#include <string>
#include <string_view>
#include <unordered_set>

namespace mir
{
    class AssetCache;
    class IScriptHost;
    class ScriptReloadWatcher;

    // Loads a script's source into the host by name; false when it is unavailable (logged)
    using ScriptLoader = std::function<bool(std::string_view scriptName)>;

    // From the cooked assets, tracked for hot reload. The cache outlives the loader
    ScriptLoader MakeScriptAssetLoader(IScriptHost &host, ScriptReloadWatcher &reload, AssetCache &assets,
        std::string assetIdRoot);

    // Destroys ref's instance, if any, so the next resolve creates a fresh one
    void ReleaseScriptInstance(IScriptHost &host, ScriptRef &ref);

    // PreUpdate, before ScriptSystem, ungated: whatever made a ScriptRef (scene load,
    // Stop, duplicate, undo, prefab publish) needs no resolve call of its own. A rename
    // replaces the instance, so its state is lost
    class ScriptResolveSystem final : public ISystem
    {
    public:
        ScriptResolveSystem(IScriptHost &host, ScriptLoader load);

        void OnStart(SystemContext &context) override;
        void OnUpdate(SystemContext &context) override;

        // a script that failed to load or to make an instance is not retried every frame;
        // an asset reload may bring it
        void ForgetFailures() { mFailed.clear(); }

    private:
        IScriptHost &mHost;
        ScriptLoader mLoad;
        Query<ScriptRef> *mQuery = nullptr;
        std::unordered_set<std::string> mLoaded;
        std::unordered_set<std::string> mFailed;
    };
}
