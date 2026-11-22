/**
 * @file MeshLoadTracker.tests.cpp
 * @author sumin.park
 * @brief Tests for renderer/MeshLoadTracker over the real asset service and a fake uploader.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer/MeshLoadTracker.h>
#include <renderer/AssetUploadCache.h>

#include <assets/AssetBlob.h>
#include <assets/AssetManifest.h>
#include <assets/AssetsModule.h>
#include <assets/AsyncAssetService.h>
#include <core/fs/FileIO.h>
#include <core/platform/Process.h>
#include <jobs/JobExecutor.h>
#include <renderer/MeshAssetDecoder.h>

#include "support/CookedMeshDir.h"
#include "support/FakeMeshUploader.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace mir;
using mir::test::CookedDir;
using mir::test::CookedMesh;
using mir::test::FakeMeshUploader;
using mir::test::kTriangle;

namespace
{
    // Zero workers, so a request decodes inside Request and its result waits for DrainCompletions
    struct Fixture
    {
        explicit Fixture(const std::vector<CookedMesh> &meshes)
            : cooked(meshes), assets(cooked.Path()), jobs(0), service(assets, jobs, 1), tracker(uploader, &service)
        {}

        std::vector<AssetId> Frame()
        {
            service.DrainCompletions();
            std::vector<AssetId> changed;
            for (const UploadKey &key : tracker.Update())
                changed.push_back(key.asset);
            return changed;
        }

        CookedDir cooked;
        AssetsModule assets;
        JobExecutor jobs;
        AsyncAssetService service;
        FakeMeshUploader uploader;
        MeshLoadTracker tracker;
    };

    AssetId Id(const char *name) { return MakeAssetId(name); }

    bool Contains(const std::vector<AssetId> &ids, AssetId id) { return std::ranges::find(ids, id) != ids.end(); }
}

TEST_CASE("A mesh goes from decoding to ready one step per frame", "[renderer][mesh-load]")
{
    Fixture f({ { "tri", std::string(kTriangle) } });

    const MeshLoad &load = f.tracker.Request(Id("tri"), "tri");
    CHECK(load.phase == MeshLoadPhase::Decoding);
    CHECK(load.mesh.IsNull());

    CHECK(f.tracker.Update().empty());
    CHECK(load.phase == MeshLoadPhase::Decoding);

    CHECK(Contains(f.Frame(), Id("tri")));
    REQUIRE(load.phase == MeshLoadPhase::Uploading);
    CHECK_FALSE(load.mesh.IsNull());
    CHECK(f.uploader.queued == 1);
    CHECK(f.uploader.meshes.at(load.mesh.mIndex).options.rayTracing);
    CHECK(f.uploader.meshes.at(load.mesh.mIndex).vertices == 3);

    f.uploader.Record(load.mesh);
    CHECK(f.Frame().empty());
    CHECK(load.phase == MeshLoadPhase::Uploading);

    f.uploader.Complete(load.mesh);
    CHECK(Contains(f.Frame(), Id("tri")));
    CHECK(load.phase == MeshLoadPhase::Ready);
    CHECK(load.bounds.mLocalMin == glm::vec3(0.0f));
    CHECK(load.bounds.mLocalMax == glm::vec3(1.0f, 1.0f, 0.0f));

    CHECK(f.Frame().empty());
}

TEST_CASE("The decoded data is shared with the uploader until it is copied", "[renderer][mesh-load]")
{
    Fixture f({ { "tri", std::string(kTriangle) } });
    const MeshLoad &load = f.tracker.Request(Id("tri"), "tri");
    f.Frame();
    REQUIRE(load.phase == MeshLoadPhase::Uploading);

    CHECK(f.uploader.meshes.at(load.mesh.mIndex).keepalive != nullptr);
    f.uploader.Record(load.mesh);
    CHECK(f.uploader.meshes.at(load.mesh.mIndex).keepalive == nullptr);
}

TEST_CASE("Two requests for one asset share one load", "[renderer][mesh-load]")
{
    Fixture f({ { "tri", std::string(kTriangle) } });

    const MeshLoad &first = f.tracker.Request(Id("tri"), "tri");
    const MeshLoad &second = f.tracker.Request(Id("tri"), "tri again");
    CHECK(&first == &second);

    f.Frame();
    CHECK(f.uploader.queued == 1);
}

TEST_CASE("Two assets requested together load independently", "[renderer][mesh-load]")
{
    Fixture f({ { "old", std::string(kTriangle) },{ "fresh", std::string(kTriangle) } });

    // an entity asked for old, then renamed itself to fresh before either finished
    f.tracker.Request(Id("old"), "old");
    f.tracker.Request(Id("fresh"), "fresh");

    const std::vector<AssetId> changed = f.Frame();
    CHECK(Contains(changed, Id("old")));
    CHECK(Contains(changed, Id("fresh")));
    REQUIRE(f.tracker.Find(Id("old")) != nullptr);
    REQUIRE(f.tracker.Find(Id("fresh")) != nullptr);
    CHECK(f.tracker.Find(Id("old"))->phase == MeshLoadPhase::Uploading);
    CHECK(f.tracker.Find(Id("fresh"))->phase == MeshLoadPhase::Uploading);
    CHECK(f.tracker.Find(Id("old"))->mesh != f.tracker.Find(Id("fresh"))->mesh);
}

TEST_CASE("A bad mesh fails once and never reaches the uploader", "[renderer][mesh-load]")
{
    Fixture f({ { "broken", "not json" } });
    const MeshLoad &load = f.tracker.Request(Id("broken"), "broken");

    CHECK(Contains(f.Frame(), Id("broken")));
    CHECK(load.phase == MeshLoadPhase::Failed);
    CHECK(load.mesh.IsNull());
    CHECK(f.uploader.queued == 0);
    CHECK(f.Frame().empty());
}

TEST_CASE("An asset the manifest does not list fails", "[renderer][mesh-load]")
{
    Fixture f({ { "tri", std::string(kTriangle) } });
    const MeshLoad &load = f.tracker.Request(Id("missing"), "missing");

    CHECK(Contains(f.Frame(), Id("missing")));
    CHECK(load.phase == MeshLoadPhase::Failed);
    CHECK(f.uploader.queued == 0);
}

TEST_CASE("A game with no asset service fails every mesh at once", "[renderer][mesh-load]")
{
    FakeMeshUploader uploader;
    MeshLoadTracker tracker(uploader, nullptr);

    const MeshLoad &load = tracker.Request(Id("tri"), "tri");

    CHECK(load.phase == MeshLoadPhase::Failed);
    CHECK(tracker.Update().empty());
}

TEST_CASE("A failed upload logs once, retires its handle and draws nothing", "[renderer][mesh-load]")
{
    Fixture f({ { "tri", std::string(kTriangle) } });
    const MeshLoad &load = f.tracker.Request(Id("tri"), "tri");
    f.Frame();
    const MeshHandle handle = load.mesh;
    REQUIRE_FALSE(handle.IsNull());

    f.uploader.Fail(handle);
    CHECK(Contains(f.Frame(), Id("tri")));

    CHECK(load.phase == MeshLoadPhase::Failed);
    CHECK(load.mesh.IsNull());
    REQUIRE(f.uploader.retired.size() == 1);
    CHECK(f.uploader.retired[0] == handle);
    CHECK(f.Frame().empty());
}

TEST_CASE("A queued upload the renderer released fails without a second retire", "[renderer][mesh-load]")
{
    Fixture f({ { "tri", std::string(kTriangle) } });
    const MeshLoad &load = f.tracker.Request(Id("tri"), "tri");
    f.Frame();

    f.uploader.ReleaseAll();
    CHECK(Contains(f.Frame(), Id("tri")));

    CHECK(load.phase == MeshLoadPhase::Failed);
    CHECK(f.uploader.retired.empty());
}

TEST_CASE("A rejected queue fails the load", "[renderer][mesh-load]")
{
    Fixture f({ { "tri", std::string(kTriangle) } });
    f.uploader.rejectQueue = true;
    const MeshLoad &load = f.tracker.Request(Id("tri"), "tri");

    f.Frame();

    CHECK(load.phase == MeshLoadPhase::Failed);
    CHECK(load.mesh.IsNull());
}

TEST_CASE("Release retires queued and uploading meshes and hands back the ready ones", "[renderer][mesh-load]")
{
    Fixture f({ { "pending", std::string(kTriangle) },{ "uploading", std::string(kTriangle) },
        { "ready", std::string(kTriangle) } });
    f.tracker.Request(Id("pending"), "pending");
    f.tracker.Request(Id("uploading"), "uploading");
    f.tracker.Request(Id("ready"), "ready");
    f.Frame();

    const MeshHandle pending = f.tracker.Find(Id("pending"))->mesh;
    const MeshHandle uploading = f.tracker.Find(Id("uploading"))->mesh;
    const MeshHandle ready = f.tracker.Find(Id("ready"))->mesh;
    f.uploader.Record(uploading);
    f.uploader.Record(ready);
    f.uploader.Complete(ready);
    f.Frame();

    std::vector<MeshHandle> readyHandles;
    f.tracker.Release(readyHandles);

    CHECK(readyHandles == std::vector<MeshHandle>{ ready });
    CHECK(f.uploader.retired.size() == 2);
    CHECK(std::ranges::find(f.uploader.retired, pending) != f.uploader.retired.end());
    CHECK(std::ranges::find(f.uploader.retired, uploading) != f.uploader.retired.end());
    CHECK(f.uploader.Count(MeshUploadState::Pending) == 0);
    CHECK(f.uploader.Count(MeshUploadState::Uploading) == 0);
    CHECK(f.tracker.Find(Id("pending")) == nullptr);
}

TEST_CASE("A load still decoding when released leaves nothing to retire", "[renderer][mesh-load]")
{
    Fixture f({ { "tri", std::string(kTriangle) } });
    f.tracker.Request(Id("tri"), "tri");

    std::vector<MeshHandle> ready;
    f.tracker.Release(ready);

    CHECK(ready.empty());
    CHECK(f.uploader.retired.empty());
    CHECK(f.tracker.Find(Id("tri")) == nullptr);

    CHECK(f.Frame().empty());
    CHECK(f.uploader.queued == 0);
}

TEST_CASE("Reset forgets loads without touching the uploader", "[renderer][mesh-load]")
{
    Fixture f({ { "tri", std::string(kTriangle) } });
    f.tracker.Request(Id("tri"), "tri");
    f.Frame();

    f.tracker.Reset();

    CHECK(f.tracker.Find(Id("tri")) == nullptr);
    CHECK(f.uploader.retired.empty());
    CHECK(f.uploader.Count(MeshUploadState::Pending) == 1);
}

TEST_CASE("Common upload cache protects independent claims and cancels the last unfinished upload",
    "[renderer][upload-cache]")
{
    Fixture f({ { "tri", std::string(kTriangle) } });
    struct View
    {
        AssetUploadPhase phase = AssetUploadPhase::Decoding;
        MeshHandle handle;
        MeshBounds bounds;
    };
    using Cache = AssetUploadCache<DecodedMesh, MeshHandle, View, &View::handle>;
    auto cache = std::make_shared<Cache>(&f.service, kMeshAssetDecoder,
        [&f](UploadKey, std::shared_ptr<const DecodedMesh> mesh)
            { return f.uploader.QueueMesh({ mesh->vertices, mesh->indices, mesh }, {}); },
        [&f](MeshHandle handle) { return f.uploader.MeshState(handle); },
        [&f](MeshHandle handle) { f.uploader.RetireMesh(handle); },
        [](UploadKey, MeshHandle, const DecodedMesh &) { return uint64_t(200); },
        [](View &view, const DecodedMesh &mesh) { view.bounds = mesh.bounds; });
    cache->Request(Id("tri"), "tri");
    AssetUploadLease<Cache> first(cache, Id("tri"));
    AssetUploadLease<Cache> second(cache, Id("tri"));
    f.service.DrainCompletions();
    cache->Update();
    REQUIRE(first.Get());
    const auto handle = first.Get()->handle;
    REQUIRE_FALSE(handle.IsNull());
    first.Reset();
    CHECK(f.uploader.MeshState(handle) == UploadState::Pending);
    second.Reset();
    CHECK(f.uploader.MeshState(handle) == UploadState::Stale);
    CHECK(cache->Empty());
    cache->Request(Id("tri"), "tri");
    AssetUploadLease<Cache> held(cache, Id("tri"));
    f.service.DrainCompletions();
    cache->Update();
    const auto ready = held.Get()->handle;
    f.uploader.Record(ready);
    f.uploader.Complete(ready);
    cache->Update();
    CHECK(held.Get()->phase == AssetUploadPhase::Ready);
    CHECK(held.Get()->bounds.mLocalMax == glm::vec3(1, 1, 0));
    cache->SetBudget(0);
    CHECK(f.uploader.MeshState(ready) == UploadState::Ready);
    held.Reset();
    CHECK(f.uploader.MeshState(ready) == UploadState::Stale);
}

TEST_CASE("A failed upload no longer holds its footprint", "[renderer][mesh-load]")
{
    Fixture f({ { "tri", std::string(kTriangle) } });
    const MeshLoad &load = f.tracker.Request(Id("tri"), "tri");
    f.tracker.Retain(Id("tri"));
    f.Frame();
    REQUIRE(load.phase == MeshLoadPhase::Uploading);
    CHECK(f.tracker.HeldBytes() > 0);
    f.uploader.Fail(load.mesh);
    f.Frame();
    REQUIRE(load.phase == MeshLoadPhase::Failed);
    CHECK(f.uploader.retired.size() == 1);
    // the memory is gone, so it must not count toward the target or evict other records
    CHECK(f.tracker.HeldBytes() == 0);
}

TEST_CASE("A ready mesh the renderer shrinks holds its new size", "[renderer][mesh-load]")
{
    Fixture f({ { "tri", std::string(kTriangle) } });
    const MeshLoad &load = f.tracker.Request(Id("tri"), "tri");
    f.tracker.Retain(Id("tri"));
    f.Frame();
    f.uploader.Record(load.mesh);
    f.uploader.Complete(load.mesh);
    f.Frame();
    REQUIRE(load.phase == MeshLoadPhase::Ready);
    const uint64_t uploaded = f.tracker.HeldBytes();
    REQUIRE(uploaded > 1);

    f.uploader.meshes.at(load.mesh.mIndex).compactedBytes = uploaded / 2;
    CHECK(f.tracker.HeldBytes() == uploaded / 2);
    CHECK(f.tracker.BytesOf(Id("tri")) == uploaded / 2);
    CHECK(f.uploader.Residency().Held(GpuCategory::Mesh) == uploaded / 2);
}
