/**
 * @file SceneModule.h
 * @author sumin.park
 * @brief The scene the World holds and how it last got there.
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
#include <utility>

namespace mir
{
    class ScenePrefabResolver;
    class World;

    enum class SceneLoadCause
    {
        FromDisk,
        New,
        // A scene was copied into another World
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
        // sceneDir is where Load reads and where Save writes once the scene is bound to it
        SceneModule(ModuleContext context, std::filesystem::path sceneDir);
        ~SceneModule();

        SceneModule(const SceneModule &) = delete;
        SceneModule &operator=(const SceneModule &) = delete;

        // the same object for the module's life; commands replace its value
        LoadedScene &Scene() { return mScene; }
        const std::filesystem::path &SceneDir() const { return mSceneDir; }

        void SetSceneDir(std::filesystem::path directory) { mSceneDir = std::move(directory); }

        // Every scene command sets it before its SceneLoad stage runs; stage functions
        // are its only readers
        const SceneLoadResult &LastLoad() const { return mLastLoad; }

        // An unbound scene is untitled: Save refuses until Bind, so the broken scene in
        // SceneDir() stays until the user chooses to replace it
        bool IsBound() const { return mBound; }
        void Bind() { mBound = true; }

        // destroys the scene's entities
        void Unload();

        // these replace the scene without unloading it first: the caller unloads. Load
        // binds; an incomplete New unbinds, and any other New keeps the binding
        void New(std::string name, bool incomplete = false);
        void LoadFromDisk(ScenePrefabResolver *resolver, bool incomplete);

        // Copies the live scene, including unsaved edits, into this module's World.
        void CopyFrom(World &source, const LoadedScene &scene, const ScenePrefabResolver *sourceResolver,
            ScenePrefabResolver *destinationResolver);

        // false on I/O failure, or while the scene is unbound
        bool Save(const ScenePrefabResolver *resolver);

    private:
        World &mWorld;
        std::filesystem::path mSceneDir;
        LoadedScene mScene;
        SceneLoadResult mLastLoad;
        bool mBound = true;
    };
}
