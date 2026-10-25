/**
 * @file TerrainRuntimeModule.cpp
 * @author sumin.park
 * @brief Streams, draws and collides terrain; its per-frame work runs at FrameBegin.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <terrain_runtime/TerrainRuntimeModule.h>

#include <terrain_runtime/TerrainRuntime.h>

#include <core/ecs/EngineMode.h>
#include <core/ecs/SystemScheduler.h>
#include <core/ecs/World.h>
#include <renderer/RendererModule.h>

namespace mir
{
    TerrainRuntimeModule::TerrainRuntimeModule(ModuleContext context, RendererModule &renderer,
        const GameProject &project)
        : mRuntime(std::make_unique<TerrainRuntime>())
    {
        mRuntime->Bind(context.world, renderer.Renderer(), project);

        // before any system runs: every mode installs finished work, and Edit picks up scene edits
        mFrameBegin = context.scheduler.AddFrameSlot(FrameSlot::FrameBegin, [this](SystemContext &frame)
            {
                mRuntime->Update();
                if (IsEditing(frame.world))
                    mRuntime->Refresh(frame.dt, false);
            });
    }

    TerrainRuntimeModule::~TerrainRuntimeModule() = default;
}
