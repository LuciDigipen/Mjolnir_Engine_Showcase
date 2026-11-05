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
    const SceneEntry *SceneCollection::Find(SceneInstanceId instance) const
    {
        if (instance == kOwnedBySession) return nullptr;
        if (primary.scene.mInstanceId == instance) return &primary;
        const auto found = additive.find(instance);
        return found == additive.end() ? nullptr : &found->second;
    }

    SceneEntry *SceneCollection::Find(SceneInstanceId instance)
    {
        return const_cast<SceneEntry *>(std::as_const(*this).Find(instance));
    }

    std::vector<SceneInstanceId> SceneCollection::Instances() const
    {
        std::vector<SceneInstanceId> result;
        if (primary.scene.mInstanceId != kOwnedBySession) result.push_back(primary.scene.mInstanceId);
        for (const auto &[instance, entry] : additive) result.push_back(instance);
        return result;
    }

    Entity SceneCollection::FindEntity(const World &world, SceneInstanceId instance, StableId stable) const
    {
        const auto *entry = Find(instance);
        if (entry == nullptr) return kNullEntity;
        const auto entity = entry->scene.mEntities.find(stable);
        if (entity == entry->scene.mEntities.end() || !world.IsAlive(entity->second) ||
            world.EntityScene(entity->second) != instance)
            return kNullEntity;
        return entity->second;
    }

    SceneModule::SceneModule(ModuleContext context, std::filesystem::path sceneDir)
        : mWorld(context.world), mScenes(mWorld.EmplaceResource<SceneCollection>()),
        mSceneDir(mScenes.primary.directory), mScene(mScenes.primary.scene)
    {
        mSceneDir = std::move(sceneDir);
        mScene.mName = "untitled";
        mWorld.SetPrimaryScene(mScene.mInstanceId);
        mLastLoad.instance = mScene.mInstanceId;
    }

    SceneModule::~SceneModule() = default;

    void SceneModule::Unload()
    {
        UnloadScene(mWorld, mScene);
        mWorld.ClearSceneResources(mScene.mInstanceId);
        mWorld.SetPrimaryScene(kOwnedBySession);
    }

    // a New over an unbound scene stays unbound, basically broken scene is on disk
    void SceneModule::New(std::string name, bool incomplete)
    {
        mScene = NewScene(std::move(name));
        mLastLoad = SceneLoadResult{ SceneLoadCause::New, incomplete, mScene.mInstanceId };
        mWorld.SetPrimaryScene(mScene.mInstanceId);
        if (incomplete)
            mBound = false;
    }

    void SceneModule::LoadFromDisk(ScenePrefabResolver *resolver, bool incomplete)
    {
        mScene = LoadScene(mWorld, mSceneDir, resolver);
        mLastLoad = SceneLoadResult{ SceneLoadCause::FromDisk, incomplete, mScene.mInstanceId };
        mWorld.SetPrimaryScene(mScene.mInstanceId);
        mBound = true;
    }

    void SceneModule::CopyFrom(World &source, const LoadedScene &scene,
        const ScenePrefabResolver *sourceResolver, ScenePrefabResolver *destinationResolver)
    {
        mScene = CopyWorld(source, scene, mWorld, sourceResolver, destinationResolver);
        mLastLoad = SceneLoadResult{ SceneLoadCause::FromSnapshot, false, mScene.mInstanceId };
        mWorld.SetPrimaryScene(mScene.mInstanceId);
    }

    void SceneModule::ReplacePrimaryScene(LoadedScene scene, std::filesystem::path directory, std::string id)
    {
        mScene = std::move(scene);
        mSceneDir = std::move(directory);
        mScenes.primary.id = std::move(id);
        mLastLoad = { SceneLoadCause::FromDisk, false, mScene.mInstanceId };
        mWorld.SetPrimaryScene(mScene.mInstanceId);
        mBound = true;
    }

    void SceneModule::Add(LoadedScene scene, std::filesystem::path directory, std::string id)
    {
        MIR_CHECK(scene.mInstanceId != kOwnedBySession && mScenes.Find(scene.mInstanceId) == nullptr,
            "SceneModule::Add needs a new, nonzero instance ID");
        if (mScene.mInstanceId == kOwnedBySession)
        {
            ReplacePrimaryScene(std::move(scene), std::move(directory), std::move(id));
            return;
        }
        const auto instance = scene.mInstanceId;
        mScenes.additive.emplace(instance, SceneEntry{ std::move(scene), std::move(directory), std::move(id) });
        mLastLoad = { SceneLoadCause::FromDisk, false, instance };
    }

    bool SceneModule::SetPrimary(SceneInstanceId instance)
    {
        if (instance == mScene.mInstanceId && instance != kOwnedBySession) return true;
        const auto found = mScenes.additive.find(instance);
        if (found == mScenes.additive.end()) return false;
        SceneEntry selected = std::move(found->second);
        mScenes.additive.erase(found);
        SceneEntry previous = std::move(mScenes.primary);
        mScenes.primary = std::move(selected);
        if (previous.scene.mInstanceId != kOwnedBySession)
        {
            const auto oldInstance = previous.scene.mInstanceId;
            mScenes.additive.emplace(oldInstance, std::move(previous));
        }
        mWorld.SetPrimaryScene(instance);
        return true;
    }

    bool SceneModule::Remove(SceneInstanceId instance)
    {
        const SceneEntry *entry = mScenes.Find(instance);
        if (entry == nullptr) return false;
        UnloadScene(mWorld, entry->scene);
        mWorld.ClearSceneResources(instance);
        if (instance == mScene.mInstanceId)
        {
            mScene = {};
            mScene.mInstanceId = kOwnedBySession;
            mSceneDir.clear();
            mScenes.primary.id.clear();
            mWorld.SetPrimaryScene(kOwnedBySession);
            if (!mScenes.additive.empty()) SetPrimary(mScenes.additive.begin()->first);
        }
        else mScenes.additive.erase(instance);
        return true;
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
