/**
 * @file TerrainGpuUploader.tests.cpp
 * @author sumin.park
 * @brief Tests for terrain_runtime/TerrainRender: terrain's uploader over the renderer's uploader interface.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <terrain_runtime/TerrainRender.h>

#include "support/FakeMeshUploader.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <utility>
#include <vector>

using namespace mir;
using mir::test::FakeMeshUploader;

namespace
{
    TerrainMeshData Triangle()
    {
        TerrainMeshData mesh;
        mesh.vertices.resize(3);
        mesh.indices = { 0, 1, 2 };
        return mesh;
    }
}

TEST_CASE("An upload queues the terrain vertices and returns the handle as an id", "[terrain_runtime][uploader]")
{
    FakeMeshUploader fake;
    TerrainGpuUploader uploader(fake);

    const TerrainGpuMeshId id = uploader.Upload(Triangle());

    REQUIRE(id != 0);
    CHECK(fake.queued == 1);
    const MeshHandle handle = TerrainGpuUploader::Handle(id);
    REQUIRE(fake.meshes.contains(handle.mIndex));
    CHECK(fake.meshes.at(handle.mIndex).vertices == 3);
    CHECK(fake.meshes.at(handle.mIndex).indices == 3);
}

TEST_CASE("An upload the renderer rejects returns no id", "[terrain_runtime][uploader]")
{
    FakeMeshUploader fake;
    fake.rejectQueue = true;
    TerrainGpuUploader uploader(fake);

    CHECK(uploader.Upload(Triangle()) == 0);
}

TEST_CASE("Ray tracing is passed to each queued mesh from the setting at that time", "[terrain_runtime][uploader]")
{
    FakeMeshUploader fake;
    TerrainGpuUploader uploader(fake);

    const MeshHandle plain = TerrainGpuUploader::Handle(uploader.Upload(Triangle()));
    uploader.SetRayTracing(true);
    const MeshHandle traced = TerrainGpuUploader::Handle(uploader.Upload(Triangle()));

    CHECK_FALSE(fake.meshes.at(plain.mIndex).options.rayTracing);
    CHECK(fake.meshes.at(traced.mIndex).options.rayTracing);
}

TEST_CASE("Renderer upload states map to terrain's", "[terrain_runtime][uploader]")
{
    FakeMeshUploader fake;
    TerrainGpuUploader uploader(fake);
    const TerrainGpuMeshId id = uploader.Upload(Triangle());
    const MeshHandle handle = TerrainGpuUploader::Handle(id);

    CHECK(uploader.State(id) == TerrainGpuMeshState::Uploading);
    fake.Record(handle);
    CHECK(uploader.State(id) == TerrainGpuMeshState::Uploading);
    fake.Complete(handle);
    CHECK(uploader.State(id) == TerrainGpuMeshState::Ready);
    fake.Fail(handle);
    CHECK(uploader.State(id) == TerrainGpuMeshState::Failed);
}

TEST_CASE("A mesh the renderer no longer knows reads as failed", "[terrain_runtime][uploader]")
{
    FakeMeshUploader fake;
    TerrainGpuUploader uploader(fake);
    const TerrainGpuMeshId id = uploader.Upload(Triangle());

    fake.ReleaseAll();

    CHECK(uploader.State(id) == TerrainGpuMeshState::Failed);
}

TEST_CASE("Retire hands the handle back to the renderer", "[terrain_runtime][uploader]")
{
    FakeMeshUploader fake;
    TerrainGpuUploader uploader(fake);
    const TerrainGpuMeshId id = uploader.Upload(Triangle());

    uploader.Retire(id);

    REQUIRE(fake.retired.size() == 1);
    CHECK(fake.retired[0] == TerrainGpuUploader::Handle(id));
}

TEST_CASE("Footprint, queued bytes, budget and retiring bytes come from the renderer", "[terrain_runtime][uploader]")
{
    FakeMeshUploader fake;
    fake.frameBudget = 12345;
    fake.retiringBytes = 678;
    TerrainGpuUploader uploader(fake);

    CHECK(uploader.FrameBudgetBytes() == 12345);
    CHECK(uploader.RetiringBytes() == 678);
    CHECK(uploader.Footprint(10, 20) == fake.MeshFootprint(10, 20, {}));

    CHECK(uploader.QueuedBytes() == 0);
    uploader.Upload(Triangle());
    CHECK(uploader.QueuedBytes() == fake.MeshFootprint(3, 3, {}));
}
