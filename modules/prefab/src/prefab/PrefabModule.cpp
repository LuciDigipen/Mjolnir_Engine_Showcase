/**
 * @file PrefabModule.cpp
 * @author sumin.park
 * @brief The prefab coordinator and the system that commits it while editing.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <prefab/PrefabModule.h>

#include <prefab/PrefabCoordinator.h>

#include <core/ecs/SystemScheduler.h>
#include <scene/SceneModule.h>
#include <core/ecs/World.h>

#include <system_error>

namespace mir
{
    PrefabModule::PrefabModule(ModuleContext context, SceneModule &scene, const std::filesystem::path &sourceDir)
        : mCoordinator(context.world.EmplaceResource<PrefabCoordinator>()), mSourceDir(sourceDir)
    {
        std::error_code ec;
        if (std::filesystem::is_directory(sourceDir, ec))
            mCoordinator.ScanSources(sourceDir);
        mCoordinator.Bind(scene.Scene());

        // pending edits reach the scene, then the published state stays put until Stop
        mPlayBegin = context.scheduler.AddStage(Stage::PlayBegin, [this](SystemContext &frame)
            {
                mCoordinator.Commit(frame.world);
                mCoordinator.SetFrozen(true);
            });

        // the scene's value changed; instances map to its stable ids
        mSceneLoad = context.scheduler.AddStage(Stage::SceneLoad, [this, &scene](SystemContext &)
            {
                mCoordinator.Bind(scene.Scene());
                if (scene.LastLoad().cause == SceneLoadCause::FromSnapshot)
                    mCoordinator.SetFrozen(false);
            });

        // first in PreUpdate: it commits at the Edit safe point, before anything reads the scene
        context.scheduler.AddSystem<PrefabSystem>(SystemPhase::PreUpdate);
    }

    void PrefabModule::Rescan()
    {
        std::error_code ec;
        if (std::filesystem::is_directory(mSourceDir, ec))
            mCoordinator.RescanSources(mSourceDir);
    }
}
