/**
 * @file PrefabModule.h
 * @author sumin.park
 * @brief The prefab coordinator and the system that commits it while editing.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/ecs/ModuleContext.h>
#include <core/Registration.h>

#include <filesystem>

namespace mir
{
    class AssetsModule;
    class PrefabCoordinator;
    class SceneModule;

    class PrefabModule
    {
    public:
        // Loads cooked assets when supplied; otherwise scans sourceDir when it exists.
        PrefabModule(ModuleContext context, SceneModule &scene, const std::filesystem::path &sourceDir,
            AssetsModule *cookedAssets = nullptr);

        PrefabModule(const PrefabModule &) = delete;
        PrefabModule &operator=(const PrefabModule &) = delete;

        PrefabCoordinator &Coordinator() { return mCoordinator; }
        const std::filesystem::path &SourceDir() const { return mSourceDir; }

        // after a prefab file under the source directory changed; waits while Play freezes the prefabs
        void Rescan();

    private:
        // a World resource, which outlives this module
        PrefabCoordinator &mCoordinator;
        std::filesystem::path mSourceDir;

        // last: they unregister before anything above is destroyed
        Registration mPlayBegin;
        Registration mSceneLoad;
    };
}
