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
#include <core/ecs/Registration.h>

#include <memory>

namespace mir
{
    class RendererModule;
    class TerrainRuntime;
    struct GameProject;

    class TerrainRuntimeModule
    {
    public:
        TerrainRuntimeModule(ModuleContext context, RendererModule &renderer, const GameProject &project);
        ~TerrainRuntimeModule();

        TerrainRuntimeModule(const TerrainRuntimeModule &) = delete;
        TerrainRuntimeModule &operator=(const TerrainRuntimeModule &) = delete;

        TerrainRuntime &Runtime() { return *mRuntime; }

    private:
        std::unique_ptr<TerrainRuntime> mRuntime;

        // last: unregisters before the runtime above is destroyed
        Registration mFrameBegin;
    };
}
