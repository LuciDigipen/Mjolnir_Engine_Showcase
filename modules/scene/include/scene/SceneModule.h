/**
 * @file SceneModule.h
 * @author sumin.park
 * @brief The scene the World holds, its Play snapshot and how it last got there.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <scene/PlaySnapshot.h>
#include <scene/SceneAsset.h>

#include <core/ecs/ModuleContext.h>

#include <filesystem>
#include <string>

namespace mir
{
    class ScenePrefabResolver;
    class World;

    enum class SceneLoadCause
    {
        FromDisk,
        New,
        // Stop put the World back as it was at Play
        FromSnapshot,
    };

    struct SceneLoadResult
    {
        SceneLoadCause cause = SceneLoadCause::New;
        // missing entity files, or a corrupt scene replaced by an empty one
        bool incomplete = false;
    };

    class SceneModule
    {
    public:
        // sceneDir is where Load reads and Save writes, also for a scene started empty
        SceneModule(ModuleContext context, std::filesystem::path sceneDir);
        ~SceneModule();

        SceneModule(const SceneModule &) = delete;
        SceneModule &operator=(const SceneModule &) = delete;

        // the same object for the module's life; commands replace its value
        LoadedScene &Scene() { return mScene; }
        const std::filesystem::path &SceneDir() const { return mSceneDir; }

        // Every scene command sets it before its SceneLoad stage runs; stage functions
        // are its only readers
        const SceneLoadResult &LastLoad() const { return mLastLoad; }

        // destroys the scene's entities
        void Unload();

        // these replace the scene without unloading it first: the caller unloads
        void New(std::string name, bool incomplete = false);
        void LoadFromDisk(ScenePrefabResolver *resolver, bool incomplete);

        // false on I/O failure
        bool Save(const ScenePrefabResolver *resolver);

        // Play keeps a snapshot; Stop destroys Play's entities and restores it
        void CaptureForPlay(const ScenePrefabResolver *resolver);
        void RestoreFromPlay(ScenePrefabResolver *resolver);

    private:
        World &mWorld;
        std::filesystem::path mSceneDir;
        LoadedScene mScene;
        PlaySnapshot mSnapshot;
        SceneLoadResult mLastLoad;
    };
}
