/**
 * @file ScriptResolveSystem.cpp
 * @author sumin.park
 * @brief Gives every ScriptRef its script instance, every frame and in every mode.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <script/ScriptResolveSystem.h>

#include <script/IScriptHost.h>
#include <script/ScriptReloadWatcher.h>

#include <assets/AssetBlob.h>
#include <assets/AssetCache.h>
#include <assets/AssetId.h>
#include <assets/AssetPath.h>
#include <core/ecs/World.h>
#include <core/log/Log.h>

#include <cstring>
#include <utility>

namespace mir
{
    ScriptLoader MakeScriptAssetLoader(IScriptHost &host, ScriptReloadWatcher &reload, AssetCache &assets,
        std::string assetIdRoot)
    {
        return [&host, &reload, &assets, idRoot = std::move(assetIdRoot)](std::string_view name)
            {
                const AssetPathResult path = MakeLogicalAssetPath(idRoot, "scripts/" + std::string(name) + ".lua");
                if (!path.Ok())
                {
                    MIR_LOG_ERROR("Script '{}': {}", name, Describe(path.error));
                    return false;
                }

                const AssetId id = MakeAssetId(path.path);
                const AssetBlobView *blob = assets.Load(id);
                if (blob == nullptr || !host.LoadScriptSource(name, AsStringView(*blob)))
                {
                    MIR_LOG_ERROR("Script '{}': could not load '{}'", name, path.path);
                    return false;
                }

                reload.Track(std::string(name), id);
                return true;
            };
    }

    void ReleaseScriptInstance(IScriptHost &host, ScriptRef &ref)
    {
        if (ref.instanceRef >= 0)
            host.DestroyInstance(ref.instanceRef);
        ref.instanceRef = -1;
        ref.started = false;
        ref.resolvedName[0] = '\0';
    }

    ScriptResolveSystem::ScriptResolveSystem(IScriptHost &host, ScriptLoader load) : mHost(host), mLoad(std::move(load))
    {
    }

    void ScriptResolveSystem::OnStart(SystemContext &context)
    {
        mQuery = &context.world.GetOrCreateQuery<ScriptRef>();
    }

    void ScriptResolveSystem::OnUpdate(SystemContext &)
    {
        mQuery->ForEach(
            [&](Entity, ScriptRef &ref)
            {
                const std::string_view name = ref.scriptName;
                if (ref.instanceRef >= 0)
                {
                    if (name == std::string_view(ref.resolvedName))
                        return;
                    ReleaseScriptInstance(mHost, ref);
                }

                if (name.empty() || mFailed.contains(std::string(name)))
                    return;

                if (!mLoaded.contains(std::string(name)))
                {
                    if (!mLoad(name))
                    {
                        mFailed.emplace(name);
                        return;
                    }
                    mLoaded.emplace(name);
                }

                ref.instanceRef = mHost.CreateInstance(name);
                std::memcpy(ref.resolvedName, ref.scriptName, sizeof(ref.resolvedName));
            });
    }
}
