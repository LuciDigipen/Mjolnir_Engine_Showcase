/**
 * @file SceneModule.cpp
 * @author sumin.park
 * @brief The scene the World holds, its Play snapshot and how it last got there.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <scene/SceneModule.h>

#include <scene/SceneIO.h>

#include <core/ecs/World.h>

#include <utility>

namespace mir
{
    SceneModule::SceneModule(ModuleContext context, std::filesystem::path sceneDir)
        : mWorld(context.world), mSceneDir(std::move(sceneDir)), mScene(NewScene("untitled"))
    {
    }

    SceneModule::~SceneModule() { DiscardSnapshot(mSnapshot); }

    void SceneModule::Unload() { UnloadScene(mWorld, mScene); }

    void SceneModule::New(std::string name, bool incomplete)
    {
        mScene = NewScene(std::move(name));
        mLastLoad = SceneLoadResult{ SceneLoadCause::New, incomplete };
    }

    void SceneModule::LoadFromDisk(ScenePrefabResolver *resolver, bool incomplete)
    {
        mScene = LoadScene(mWorld, mSceneDir, resolver);
        mLastLoad = SceneLoadResult{ SceneLoadCause::FromDisk, incomplete };
    }

    bool SceneModule::Save(const ScenePrefabResolver *resolver)
    {
        if (!SaveScene(mWorld, mSceneDir, mScene, resolver))
            return false;
        mScene.mDirty = false;
        return true;
    }

    void SceneModule::CaptureForPlay() { mSnapshot = CaptureWorld(mWorld); }

    void SceneModule::RestoreFromPlay()
    {
        mScene = RestoreWorld(mWorld, mSnapshot);
        mLastLoad = SceneLoadResult{ SceneLoadCause::FromSnapshot, false };
    }
}
