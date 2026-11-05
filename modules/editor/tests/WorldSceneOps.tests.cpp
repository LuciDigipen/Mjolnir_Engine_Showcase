/**
 * @file WorldSceneOps.tests.cpp
 * @author sumin.park
 * @brief Tests that scene edits reach the scene owning their entity, with additive and session-owned entities.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <editor/PrefabAuthoringAdapter.h>
#include <editor/PrefabWorkspace.h>
#include <editor/WorldSceneOps.h>

#include <core/ecs/ComponentRegistry.h>
#include <core/ecs/TransformHierarchy.h>
#include <core/ecs/World.h>
#include <prefab/PrefabCoordinator.h>
#include <scene/SceneAsset.h>
#include <scene/SceneModule.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>

namespace
{
    using namespace mir;

    struct ScratchDir
    {
        std::filesystem::path mPath;

        ScratchDir()
        {
            static std::atomic<int> next{ 0 };
            mPath = std::filesystem::temp_directory_path() / ("mir_world_scene_ops_" + std::to_string(next++));
            std::filesystem::create_directories(mPath);
        }

        ~ScratchDir()
        {
            std::error_code ec;
            std::filesystem::remove_all(mPath, ec);
        }
    };

    // A World holding a primary scene and one additive scene, the shape a runtime World has during Play.
    struct TwoScenes
    {
        World mWorld;
        ScratchDir mRoot;
        PrefabCoordinator mCoordinator;
        std::unique_ptr<PrefabAuthoringAdapter> mAdapter;
        std::unique_ptr<PrefabWorkspace> mPrefabs;
        std::unique_ptr<WorldSceneOps> mOps;
        SceneCollection *mScenes = nullptr;
        SceneInstanceId mExtra = kOwnedBySession;

        TwoScenes()
        {
            RegisterCoreComponents();
            mScenes = &mWorld.EmplaceResource<SceneCollection>();
            mScenes->primary.scene = NewScene("primary");
            mWorld.SetPrimaryScene(mScenes->primary.scene.mInstanceId);

            LoadedScene extra = NewScene("extra");
            mExtra = extra.mInstanceId;
            mScenes->additive.emplace(mExtra, SceneEntry{ std::move(extra), {}, "extra" });

            mAdapter = std::make_unique<PrefabAuthoringAdapter>(mWorld, mCoordinator, mRoot.mPath, mRoot.mPath);
            mPrefabs = std::make_unique<PrefabWorkspace>(mCoordinator, *mAdapter);
            mOps = std::make_unique<WorldSceneOps>(mWorld, *mPrefabs, mRoot.mPath);
        }

        LoadedScene &Primary() { return mScenes->primary.scene; }
        LoadedScene &Extra() { return mScenes->additive.at(mExtra).scene; }

        bool Registered(const LoadedScene &scene, Entity entity) const
        {
            for (const auto &[id, registered] : scene.mEntities)
                if (registered == entity)
                    return true;
            return false;
        }
    };
}

TEST_CASE("Deleting an additive scene's entity updates that scene's records", "[editor][scene-ops]")
{
    TwoScenes fixture;
    const Entity entity = CreateSceneEntity(fixture.mWorld, fixture.Extra());
    const std::size_t primaryCount = fixture.Primary().mEntities.size();

    REQUIRE(fixture.mOps->Destroy(entity));

    CHECK_FALSE(fixture.mWorld.IsAlive(entity));
    CHECK(fixture.Extra().mEntities.empty());
    CHECK(fixture.Primary().mEntities.size() == primaryCount);
}

TEST_CASE("Duplicating an additive scene's entity keeps the copy in that scene", "[editor][scene-ops]")
{
    TwoScenes fixture;
    const Entity source = CreateSceneEntity(fixture.mWorld, fixture.Extra());

    const Entity copy = fixture.mOps->Duplicate(source);

    REQUIRE(fixture.mWorld.IsAlive(copy));
    CHECK(fixture.mWorld.EntityScene(copy) == fixture.mExtra);
    CHECK(fixture.Registered(fixture.Extra(), copy));
    CHECK_FALSE(fixture.Registered(fixture.Primary(), copy));
}

TEST_CASE("Undoing the delete of an additive scene's entity restores it into that scene", "[editor][scene-ops]")
{
    TwoScenes fixture;
    const Entity entity = CreateSceneEntity(fixture.mWorld, fixture.Extra());
    const CapturedSubtree captured = fixture.mOps->Capture(entity);
    REQUIRE(fixture.mOps->Destroy(entity));

    const std::vector<Entity> restored = fixture.mOps->Restore(captured, kNullEntity, kNullEntity);

    REQUIRE(restored.size() == 1);
    CHECK(fixture.mWorld.EntityScene(restored.front()) == fixture.mExtra);
    CHECK(fixture.Registered(fixture.Extra(), restored.front()));
    CHECK(fixture.Primary().mEntities.empty());
}

TEST_CASE("An entity no scene owns is edited directly and stays owned by the session", "[editor][scene-ops]")
{
    TwoScenes fixture;
    Entity spawned = kNullEntity;
    {
        const World::SceneScope session(fixture.mWorld, kOwnedBySession);
        spawned = fixture.mWorld.CreateEntity();
    }

    const Entity copy = fixture.mOps->Duplicate(spawned);
    REQUIRE(fixture.mWorld.IsAlive(copy));
    CHECK(fixture.mWorld.EntityScene(copy) == kOwnedBySession);
    CHECK(fixture.Primary().mEntities.empty());
    CHECK(fixture.Extra().mEntities.empty());

    CHECK(fixture.mOps->Destroy(spawned));
    CHECK_FALSE(fixture.mWorld.IsAlive(spawned));
}

TEST_CASE("Reparenting under an entity from another scene is refused", "[editor][scene-ops]")
{
    TwoScenes fixture;
    const Entity primaryParent = CreateSceneEntity(fixture.mWorld, fixture.Primary());
    const Entity primaryChild = CreateSceneEntity(fixture.mWorld, fixture.Primary());
    const Entity extraChild = CreateSceneEntity(fixture.mWorld, fixture.Extra());

    const Entity sameScene[]{ primaryChild };
    const Entity otherScene[]{ extraChild };
    CHECK(fixture.mOps->CanReparent(sameScene, primaryParent));
    CHECK_FALSE(fixture.mOps->CanReparent(otherScene, primaryParent));
    CHECK(fixture.mOps->CanReparent(otherScene, kNullEntity));
}
