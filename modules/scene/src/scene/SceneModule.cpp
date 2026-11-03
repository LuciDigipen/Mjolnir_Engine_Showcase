/**
 * @file SceneModule.cpp
 * @author sumin.park
 * @brief The scene the World holds and how it last got there.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <scene/SceneModule.h>

#include <scene/SceneIO.h>

#include <core/ecs/World.h>
#include <core/log/Log.h>

#include <utility>

namespace mir
{
    SceneModule::SceneModule(ModuleContext context, std::filesystem::path sceneDir)
        : mWorld(context.world), mSceneDir(std::move(sceneDir)), mScene(NewScene("untitled"))
    {
    }

    SceneModule::~SceneModule() = default;

    void SceneModule::Unload() { UnloadScene(mWorld, mScene); }

    // a New over an unbound scene stays unbound, basically broken scene is on disk
    void SceneModule::New(std::string name, bool incomplete)
    {
        mScene = NewScene(std::move(name));
        mLastLoad = SceneLoadResult{ SceneLoadCause::New, incomplete };
        if (incomplete)
            mBound = false;
    }

    void SceneModule::LoadFromDisk(ScenePrefabResolver *resolver, bool incomplete)
    {
        mScene = LoadScene(mWorld, mSceneDir, resolver);
        mLastLoad = SceneLoadResult{ SceneLoadCause::FromDisk, incomplete };
        mBound = true;
    }

    void SceneModule::CopyFrom(World &source, const LoadedScene &scene, ScenePrefabResolver *resolver)
    {
        mScene = CopyWorld(source, scene, mWorld, resolver);
        mLastLoad = SceneLoadResult{ SceneLoadCause::FromSnapshot, false };
    }

    bool SceneModule::Save(const ScenePrefabResolver *resolver)
    {
        if (!mBound)
        {
            MIR_LOG_ERROR("SceneModule::Save: the untitled scene is not bound to '{}'", mSceneDir.string());
            return false;
        }

        if (!SaveScene(mWorld, mSceneDir, mScene, resolver))
            return false;
        mScene.mDirty = false;
        return true;
    }

}
