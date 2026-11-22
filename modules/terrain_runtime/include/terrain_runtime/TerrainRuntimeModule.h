/**
 * @file TerrainRuntimeModule.h
 * @author sumin.park
 * @brief Streams, draws and collides terrain; its per-frame work runs at FrameBegin.
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
    class RendererModule;
    class TerrainRuntime;
    class TerrainRuntimeCache;
    class JobExecutor;
    struct GameProject;

    class TerrainRuntimeModule
    {
    public:
        TerrainRuntimeModule(ModuleContext context, RendererModule &renderer, const GameProject &project, const AssetResolver &resolver,
                             JobExecutor &jobs, bool updateInPlay = true,
                             std::shared_ptr<TerrainRuntimeCache> cache = {});
        ~TerrainRuntimeModule();

        TerrainRuntimeModule(const TerrainRuntimeModule &) = delete;
        TerrainRuntimeModule &operator=(const TerrainRuntimeModule &) = delete;

        TerrainRuntime &Runtime() { return *mRuntime; }

        // The payload and GPU mesh cache both Worlds share; null when the project streams no terrain.
        std::shared_ptr<TerrainRuntimeCache> Cache() const;

    private:
        std::unique_ptr<TerrainRuntime> mRuntime;

        // last: they unregister before the runtime above is destroyed
        Registration mFrameBegin;
        Registration mSceneUnload;
        Registration mSceneLoad;
        Registration mPlayBegin;
    };
}
