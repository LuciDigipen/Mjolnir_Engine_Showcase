/**
 * @file MeshResolveSystem.tests.cpp
 * @author sumin.park
 * @brief Tests for renderer/MeshResolveSystem over the real asset service and a fake resolve host.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer/MeshResolveSystem.h>

#include <assets/AssetResolver.h>
#include <assets/AssetsModule.h>
#include <assets/AsyncAssetService.h>
#include <core/Registration.h>
#include <core/ecs/CommandBuffer.h>
#include <core/ecs/World.h>
#include <jobs/JobExecutor.h>
#include <renderer/MeshAssetDecoder.h>
#include <renderer/Shapes.h>
#include <renderer/components/MeshBounds.h>
#include <renderer/components/MeshRenderer.h>

#include "support/CookedMeshDir.h"
#include "support/FakeMeshResolveHost.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace mir;
using mir::test::CookedDir;
using mir::test::CookedMesh;
using mir::test::FakeMeshResolveHost;
using mir::test::kTriangle;

namespace
{
    // Twice the size of kTriangle, so the two meshes' bounds tell them apart
    constexpr std::string_view kBigTriangle = R"({
        "vertices": [
            { "position": [0, 0, 0], "color": [1, 1, 1], "normal": [0, 1, 0] },
            { "position": [4, 0, 0], "color": [1, 1, 1], "normal": [0, 1, 0] },
            { "position": [4, 4, 0], "color": [1, 1, 1], "normal": [0, 1, 0] }
        ],
        "indices": [0, 1, 2]
    })";

    // Meshes are named under "game", and an entity names them relative to it
    const std::vector<CookedMesh> kMeshes = { { "game/tri", std::string(kTriangle) },
        { "game/big", std::string(kBigTriangle) }, { "game/broken", "not json" } };

    // One World with its resolve system, as the scene or a prefab preview has
    struct Scene
    {
        Scene(FakeMeshResolveHost &host, AsyncAssetService *service, const AssetResolver &resolver)
            : system(host, service, resolver),
            hooks(world.SetComponentHooks<MeshRenderer>(
                { .onRemove = [this](World &, Entity entity) { system.Forget(entity); } }))
        {}

        Entity Spawn(const char *meshName)
        {
            const Entity entity = world.CreateEntity();
            world.AddComponent<MeshRenderer>(entity, MeshRenderer{});
            Rename(entity, meshName);
            return entity;
        }

        void Rename(Entity entity, const char *meshName)
        {
            MeshRenderer *renderer = world.GetComponent<MeshRenderer>(entity);
            std::snprintf(renderer->meshName, sizeof(renderer->meshName), "%s", meshName);
        }

        MeshHandle MeshOf(Entity entity) { return world.GetComponent<MeshRenderer>(entity)->mesh; }
        const MeshBounds *BoundsOf(Entity entity) { return world.GetComponent<MeshBounds>(entity); }

        void Update() { system.OnUpdate(context); }

        World world;
        CommandBuffer commands;
        SystemContext context{ world, commands, 0.016f, 0.0, 0 };
        MeshResolveSystem system;
        Registration hooks;
    };

    // The cooked directory comes first, so the module built after it finds the manifest or its absence
    struct CookedStart
    {
        explicit CookedStart(bool cook)
        {
            if (cook)
                cooked.Cook(kMeshes);
        }

        CookedDir cooked;
    };

    // Zero workers, so a request decodes inside Request and its result waits for DrainCompletions. A game
    // that is not cooked has no manifest, like one before its first import.
    struct Fixture : CookedStart
    {
        explicit Fixture(bool cook = true)
            : CookedStart(cook), assets(cooked.Path()), jobs(0), service(assets, jobs, 1), resolver(assets, "game"),
            scene(host, &service, resolver)
        {}

        // One drain and one update, as a frame does
        void Frame()
        {
            service.DrainCompletions();
            scene.Update();
        }

        // The frame that completes every upload the host has recorded
        void FinishUploads()
        {
            std::vector<MeshHandle> handles;
            for (const auto &[index, mesh] : host.uploader.meshes)
                handles.push_back(MeshHandle{ index, 1 });
            for (const MeshHandle handle : handles)
            {
                if (host.uploader.MeshState(handle) == MeshUploadState::Pending)
                    host.uploader.Record(handle);
                host.uploader.Complete(handle);
            }
        }

        AssetsModule assets;
        JobExecutor jobs;
        AsyncAssetService service;
        AssetResolver resolver;
        FakeMeshResolveHost host;
        Scene scene;
    };
}

TEST_CASE("A mesh entity draws when its upload is queued and gains bounds when it is ready", "[renderer][mesh-resolve]")
{
    Fixture f;
    const Entity entity = f.scene.Spawn("tri");

    // the decode runs inside the first frame, its result shows after the next drain
    f.Frame();
    CHECK(f.scene.MeshOf(entity).IsNull());
    CHECK(f.scene.BoundsOf(entity) == nullptr);

    f.Frame();
    CHECK_FALSE(f.scene.MeshOf(entity).IsNull());
    CHECK(f.scene.BoundsOf(entity) == nullptr);

    f.FinishUploads();
    f.Frame();
    const MeshBounds *bounds = f.scene.BoundsOf(entity);
    REQUIRE(bounds != nullptr);
    CHECK(bounds->mLocalMax == glm::vec3(1.0f, 1.0f, 0.0f));
    CHECK(f.host.uploader.queued == 1);
}

TEST_CASE("An entity renamed while its first mesh loads ends up with the new mesh only", "[renderer][mesh-resolve]")
{
    Fixture f;
    const Entity entity = f.scene.Spawn("tri");
    f.Frame();

    f.scene.Rename(entity, "big");
    f.Frame();
    // the old mesh still uploads for anyone who names it, the entity no longer takes it
    CHECK(f.host.uploader.queued == 1);
    CHECK(f.scene.MeshOf(entity).IsNull());
    CHECK(f.scene.BoundsOf(entity) == nullptr);

    f.Frame();
    REQUIRE(f.host.uploader.queued == 2);
    const MeshHandle bigHandle = f.scene.MeshOf(entity);
    REQUIRE_FALSE(bigHandle.IsNull());
    // the first upload was queued first, so it holds the lower index
    CHECK(bigHandle.mIndex == 1);

    f.FinishUploads();
    f.Frame();
    REQUIRE(f.scene.BoundsOf(entity) != nullptr);
    CHECK(f.scene.BoundsOf(entity)->mLocalMax == glm::vec3(4.0f, 4.0f, 0.0f));
    CHECK(f.host.uploader.retired.empty());
}

TEST_CASE("Entities that name one mesh share one load and one set of bounds", "[renderer][mesh-resolve]")
{
    Fixture f;
    const Entity first = f.scene.Spawn("tri");
    const Entity second = f.scene.Spawn("tri");
    f.Frame();
    f.Frame();

    CHECK(f.host.uploader.queued == 1);
    CHECK(f.scene.MeshOf(first) == f.scene.MeshOf(second));
    CHECK_FALSE(f.scene.MeshOf(first).IsNull());

    f.FinishUploads();
    f.Frame();
    CHECK(f.scene.BoundsOf(first) != nullptr);
    CHECK(f.scene.BoundsOf(second) != nullptr);

    // one entity going away leaves the mesh to the other
    f.scene.world.DestroyEntity(first);
    f.Frame();
    CHECK(f.host.uploader.retired.empty());
    CHECK(f.host.uploader.MeshState(f.scene.MeshOf(second)) == MeshUploadState::Ready);
    CHECK(f.scene.BoundsOf(second) != nullptr);
}

TEST_CASE("An entity removed while its mesh loads leaves the load for the next entity", "[renderer][mesh-resolve]")
{
    Fixture f;
    const Entity entity = f.scene.Spawn("tri");
    f.Frame();
    f.scene.world.DestroyEntity(entity);

    f.Frame();
    f.FinishUploads();
    f.Frame();
    CHECK(f.host.uploader.queued == 1);
    CHECK(f.host.uploader.retired.empty());

    // a later entity finds the finished mesh at once, handle and bounds in its first frame
    const Entity later = f.scene.Spawn("tri");
    f.Frame();
    CHECK_FALSE(f.scene.MeshOf(later).IsNull());
    CHECK(f.scene.BoundsOf(later) != nullptr);
    CHECK(f.host.uploader.queued == 1);
}

TEST_CASE("Renaming between a loaded asset and a builtin shape swaps handle and bounds", "[renderer][mesh-resolve]")
{
    Fixture f;
    const Entity entity = f.scene.Spawn("tri");
    f.Frame();
    f.Frame();
    f.FinishUploads();
    f.Frame();
    const MeshHandle loaded = f.scene.MeshOf(entity);
    REQUIRE(f.scene.BoundsOf(entity) != nullptr);

    // a builtin needs no load, so it shows in the frame that renames it
    f.scene.Rename(entity, std::string(kCubeMeshName).c_str());
    f.Frame();
    CHECK(f.scene.MeshOf(entity).mGeneration == FakeMeshResolveHost::kSyncGeneration);
    REQUIRE(f.scene.BoundsOf(entity) != nullptr);
    CHECK(f.scene.BoundsOf(entity)->mLocalMax == ComputeMeshBounds(MakeCube().vertices).mLocalMax);

    // the loaded mesh was kept, so going back needs no second upload
    f.scene.Rename(entity, "tri");
    f.Frame();
    CHECK(f.scene.MeshOf(entity) == loaded);
    CHECK(f.scene.BoundsOf(entity)->mLocalMax == glm::vec3(1.0f, 1.0f, 0.0f));
    CHECK(f.host.uploader.queued == 1);

    // an empty name is no mesh
    f.scene.Rename(entity, "");
    f.Frame();
    CHECK(f.scene.MeshOf(entity).IsNull());
    CHECK(f.scene.BoundsOf(entity) == nullptr);
}

TEST_CASE("A mesh that cannot load leaves its entity undrawn", "[renderer][mesh-resolve]")
{
    Fixture f;
    const Entity broken = f.scene.Spawn("broken");
    const Entity unlisted = f.scene.Spawn("nowhere");
    f.Frame();
    f.Frame();

    CHECK(f.scene.MeshOf(broken).IsNull());
    CHECK(f.scene.BoundsOf(broken) == nullptr);
    CHECK(f.scene.MeshOf(unlisted).IsNull());
    CHECK(f.host.uploader.queued == 0);
}

TEST_CASE("Releasing the system retires unfinished uploads and releases the ready ones", "[renderer][mesh-resolve]")
{
    Fixture f;
    f.scene.Spawn("tri");
    f.scene.Spawn("big");
    f.Frame();
    f.Frame();
    REQUIRE(f.host.uploader.queued == 2);
    // only tri completes
    f.host.uploader.Record(MeshHandle{ 0, 1 });
    f.host.uploader.Complete(MeshHandle{ 0, 1 });
    f.Frame();

    f.scene.system.ReleaseResources();

    CHECK(f.host.releasedMeshes.size() == 1);
    CHECK(f.host.uploader.meshes.empty());
}

TEST_CASE("The first cook makes the meshes of a scene and a preview drawable without reopening them",
    "[renderer][mesh-resolve]")
{
    // a game that started without a manifest, with the scene and one prefab preview already holding meshes
    Fixture f(false);
    Scene preview(f.host, &f.service, f.resolver);
    const Entity sceneEntity = f.scene.Spawn("tri");
    const Entity previewEntity = preview.Spawn("tri");
    for (int frame = 0; frame < 3; ++frame)
    {
        f.service.DrainCompletions();
        f.scene.Update();
        preview.Update();
    }
    REQUIRE(f.scene.MeshOf(sceneEntity).IsNull());
    REQUIRE(preview.MeshOf(previewEntity).IsNull());

    // the import applies: the new manifest is swapped in, then every resolve system commits
    f.cooked.Cook(kMeshes);
    f.assets.BeginReload();
    f.assets.CommitReload();
    f.scene.system.CommitReload();
    preview.system.CommitReload();

    for (int frame = 0; frame < 3; ++frame)
    {
        f.service.DrainCompletions();
        f.scene.Update();
        preview.Update();
        f.FinishUploads();
    }

    CHECK_FALSE(f.scene.MeshOf(sceneEntity).IsNull());
    CHECK(f.scene.BoundsOf(sceneEntity) != nullptr);
    CHECK_FALSE(preview.MeshOf(previewEntity).IsNull());
    CHECK(preview.BoundsOf(previewEntity) != nullptr);
}
