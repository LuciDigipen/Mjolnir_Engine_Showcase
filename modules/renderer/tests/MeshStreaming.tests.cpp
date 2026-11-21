/**
 * @file MeshStreaming.tests.cpp
 * @author Jackey Cen
 * @brief Tests for renderer/MeshStreaming.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer/MeshStreaming.h>
#include <renderer/TextureUpload.h>

#include <catch2/catch_test_macros.hpp>

#include <glm/vec3.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace mir;

namespace
{
    constexpr uint64_t kAlign = MeshUploadQueue::kStagingAlignment;

    MeshHandle Mesh(uint32_t index, uint32_t generation = 1)
    {
        return MeshHandle{index, generation};
    }

    std::vector<uint32_t> Slots(const std::vector<MeshUploadQueue::Admitted> &admitted)
    {
        std::vector<uint32_t> slots;
        for (const MeshUploadQueue::Admitted &entry : admitted)
            slots.push_back(entry.item.mesh.mIndex);
        return slots;
    }
} // namespace

TEST_CASE("Staging ring space comes back only when the frame that copied from it completes", "[renderer][streaming]")
{
    StagingRing ring(4 * kAlign);
    REQUIRE(ring.Allocate(kAlign, kAlign, 10) == uint64_t(0));
    REQUIRE(ring.Allocate(2 * kAlign, kAlign, 10) == kAlign);
    REQUIRE(ring.Allocate(kAlign, kAlign, 11) == 3 * kAlign);
    CHECK(ring.Used() == 4 * kAlign);
    CHECK_FALSE(ring.Allocate(1, kAlign, 12));

    ring.Release(9);
    CHECK(ring.Used() == 4 * kAlign);
    CHECK_FALSE(ring.Allocate(1, kAlign, 12));

    ring.Release(10);
    CHECK(ring.Used() == kAlign);
    REQUIRE(ring.Allocate(2 * kAlign, kAlign, 12) == uint64_t(0));
    CHECK_FALSE(ring.Allocate(2 * kAlign, kAlign, 12));
    REQUIRE(ring.Allocate(kAlign, kAlign, 12) == 2 * kAlign);

    ring.Release(12);
    CHECK(ring.Used() == 0);
}

TEST_CASE("A staging block never straddles the end of the ring", "[renderer][streaming]")
{
    StagingRing ring(4 * kAlign);
    REQUIRE(ring.Allocate(3 * kAlign, kAlign, 1) == uint64_t(0));
    ring.Release(1);
    REQUIRE(ring.Allocate(kAlign + 1, kAlign, 2) == uint64_t(0));
    REQUIRE(ring.Allocate(10, kAlign, 2) == 2 * kAlign);
    CHECK_FALSE(ring.Allocate(4 * kAlign + 1, kAlign, 3));
    CHECK_FALSE(StagingRing(0).Allocate(1, kAlign, 1));
}

TEST_CASE("Each frame admits queued meshes in order within its byte budget, the first even past it",
          "[renderer][streaming]")
{
    MeshUploadQueue queue({.stagingBytes = 64 * kAlign, .frameBytes = 1000});
    queue.Push({Mesh(0), kAlign, 600});
    queue.Push({Mesh(1), kAlign, 300});
    queue.Push({Mesh(2), kAlign, 200});
    queue.Push({Mesh(3), kAlign, 5000});
    queue.Push({Mesh(4), kAlign, 100});
    CHECK(queue.PendingBytes() == 6200);

    CHECK(Slots(queue.Admit(3)) == std::vector<uint32_t>{0, 1});
    CHECK(queue.LastFrameBytes() == 900);
    CHECK(Slots(queue.Admit(4)) == std::vector<uint32_t>{2});
    CHECK(Slots(queue.Admit(5)) == std::vector<uint32_t>{3});
    CHECK(queue.LastFrameBytes() == 5000);
    CHECK(Slots(queue.Admit(6)) == std::vector<uint32_t>{4});
    CHECK(queue.Admit(7).empty());
    CHECK(queue.LastFrameBytes() == 0);
    CHECK(queue.Pending() == 0);
    CHECK(queue.Uploading() == 5);
    CHECK(queue.UploadingBytes() == 6200);
}

TEST_CASE("A frame budget change applies from the next admission and leaves queued and in-flight meshes alone",
          "[renderer][streaming]")
{
    MeshUploadQueue queue({.stagingBytes = 64 * kAlign, .frameBytes = 100});
    for (uint32_t i = 0; i < 6; ++i)
        queue.Push({Mesh(i), kAlign, 100});
    CHECK(Slots(queue.Admit(1)) == std::vector<uint32_t>{0});

    queue.SetFrameBytes(300);
    CHECK(queue.Desc().frameBytes == 300);
    CHECK(queue.Desc().stagingBytes == 64 * kAlign);
    CHECK(queue.Uploading() == 1);
    CHECK(queue.Pending() == 5);
    CHECK(Slots(queue.Admit(2)) == std::vector<uint32_t>{1, 2, 3});
    CHECK(queue.LastFrameBytes() == 300);

    // The first mesh of a frame is still admitted.
    queue.SetFrameBytes(10);
    CHECK(Slots(queue.Admit(3)) == std::vector<uint32_t>{4});
    CHECK(Slots(queue.Admit(4)) == std::vector<uint32_t>{5});
    CHECK(queue.Complete(4).size() == 6);
}

TEST_CASE("An empty staging ring admits large uploads after completion or clearing", "[renderer][streaming]")
{
    MeshUploadQueue queue({.stagingBytes = 4 * kAlign, .frameBytes = 4 * kAlign});
    queue.Push({Mesh(0), 2 * kAlign, 2 * kAlign});
    REQUIRE(Slots(queue.Admit(1)) == std::vector<uint32_t>{0});

    SECTION("The previous copy completes")
    {
        REQUIRE(queue.Complete(1) == std::vector<MeshHandle>{Mesh(0)});
    }
    SECTION("The queue is cleared at scene unload")
    {
        queue.Clear();
    }
    REQUIRE(queue.StagingUsed() == 0);

    // Larger than either contiguous portion at the old cursor, but smaller than the empty ring.
    queue.Push({Mesh(1), 3 * kAlign, 3 * kAlign});
    queue.Push({Mesh(2), kAlign, kAlign});
    REQUIRE(Slots(queue.Admit(2)) == std::vector<uint32_t>{1, 2});
    CHECK(queue.Pending() == 0);
    CHECK(queue.StagingUsed() == 4 * kAlign);
    REQUIRE(queue.Complete(2).size() == 2);

    // Alignment padding after a short copy must not prevent a full-capacity upload either.
    queue.Push({Mesh(3), 1, 1});
    REQUIRE(queue.Admit(3).size() == 1);
    REQUIRE(queue.Complete(3).size() == 1);
    queue.Push({Mesh(4), 4 * kAlign, 4 * kAlign});
    REQUIRE(Slots(queue.Admit(4)) == std::vector<uint32_t>{4});
    REQUIRE(queue.Complete(4).size() == 1);
    CHECK(queue.StagingUsed() == 0);
}

TEST_CASE("A full staging ring holds later meshes back until earlier frames complete", "[renderer][streaming]")
{
    MeshUploadQueue queue({.stagingBytes = 4 * kAlign, .frameBytes = 1u << 20});
    for (uint32_t i = 0; i < 6; ++i)
        queue.Push({Mesh(i), kAlign, 10});

    CHECK(Slots(queue.Admit(3)) == std::vector<uint32_t>{0, 1, 2, 3});
    CHECK(queue.StagingUsed() == 4 * kAlign);
    CHECK(queue.Admit(4).empty());
    CHECK(queue.Complete(2).empty());
    CHECK(queue.Admit(4).empty());

    const std::vector<MeshHandle> done = queue.Complete(3);
    CHECK(done.size() == 4);
    CHECK(queue.UploadingBytes() == 0);
    CHECK(Slots(queue.Admit(4)) == std::vector<uint32_t>{4, 5});
}

TEST_CASE("A mesh larger than the staging ring is admitted to a staging buffer of its own", "[renderer][streaming]")
{
    MeshUploadQueue queue({.stagingBytes = 4 * kAlign, .frameBytes = 1u << 20});
    queue.Push({Mesh(0), kAlign, 10});
    queue.Push({Mesh(1), 8 * kAlign, 10});
    const std::vector<MeshUploadQueue::Admitted> admitted = queue.Admit(3);
    REQUIRE(admitted.size() == 2);
    CHECK(admitted[0].stagingOffset == uint64_t(0));
    CHECK_FALSE(admitted[1].stagingOffset);
    CHECK(queue.StagingUsed() == kAlign);
}

TEST_CASE("Only a mesh not admitted yet can be canceled", "[renderer][streaming]")
{
    MeshUploadQueue queue({.stagingBytes = 64 * kAlign, .frameBytes = 100});
    queue.Push({Mesh(0), kAlign, 100});
    queue.Push({Mesh(1), kAlign, 100});
    queue.Push({Mesh(2), kAlign, 100});
    CHECK(Slots(queue.Admit(3)) == std::vector<uint32_t>{0});

    CHECK_FALSE(queue.Cancel(Mesh(0)));
    CHECK_FALSE(queue.Cancel(Mesh(1, 2)));
    CHECK(queue.Cancel(Mesh(1)));
    CHECK(queue.PendingBytes() == 100);
    CHECK(Slots(queue.Admit(4)) == std::vector<uint32_t>{2});

    const std::vector<MeshHandle> done = queue.Complete(4);
    REQUIRE(done.size() == 2);
    CHECK(done[0] == Mesh(0));
    CHECK(done[1] == Mesh(2));
}

TEST_CASE("Clearing the queue forgets queued and in-flight meshes and frees the ring", "[renderer][streaming]")
{
    MeshUploadQueue queue({.stagingBytes = 4 * kAlign, .frameBytes = 1u << 20});
    for (uint32_t i = 0; i < 6; ++i)
        queue.Push({Mesh(i), kAlign, 10});
    queue.Admit(3);
    queue.Clear();
    CHECK(queue.Pending() == 0);
    CHECK(queue.Uploading() == 0);
    CHECK(queue.StagingUsed() == 0);
    CHECK(queue.Complete(10).empty());
}

TEST_CASE("Retired objects are released once every frame that may use them completes", "[renderer][streaming]")
{
    FrameRetireList<std::string> retired;
    retired.Push(5, 100, "a");
    retired.Push(6, 20, "b");
    retired.Push(6, 3, "c");
    CHECK(retired.Bytes() == 123);

    std::vector<std::string> released;
    const auto release = [&](std::string &value) { released.push_back(value); };
    retired.Collect(4, release);
    CHECK(released.empty());
    retired.Collect(5, release);
    CHECK(released == std::vector<std::string>{"a"});
    CHECK(retired.Bytes() == 23);
    CHECK(retired.Size() == 2);

    retired.ReleaseAll(release);
    CHECK(released == std::vector<std::string>{"a", "b", "c"});
    CHECK(retired.Bytes() == 0);
    CHECK(retired.Size() == 0);
}

// These cover the source's own ownership only. The renderer's release points (MeshUploader Record, Forget and
// Clear) need a device and have no unit test, the editor and game smoke runs reach them.
TEST_CASE("MeshUploadSource::Own spans its data and frees it with the last keepalive", "[renderer][streaming]")
{
    std::vector<Vertex> vertices(3);
    vertices[2].pos = glm::vec3(1.0f, 2.0f, 3.0f);
    MeshUploadSource source = MeshUploadSource::Own(std::move(vertices), { 0, 1, 2 });

    CHECK(source.vertices.size() == 3);
    CHECK(source.indices.size() == 3);
    CHECK(source.vertices[2].pos == glm::vec3(1.0f, 2.0f, 3.0f));
    CHECK(source.keepalive.use_count() == 1);

    const std::weak_ptr<const void> watcher = source.keepalive;
    source.keepalive.reset();
    CHECK(watcher.expired());
}

TEST_CASE("Resetting one keepalive leaves another holder of the data alone", "[renderer][streaming]")
{
    MeshUploadSource source = MeshUploadSource::Own(std::vector<Vertex>(2), { 0, 1, 0 });
    const std::shared_ptr<const void> cache = source.keepalive;

    source.keepalive.reset();

    CHECK(cache.use_count() == 1);
}

TEST_CASE("A 4K texture streams bounded bands through a smaller ring", "[renderer][streaming][texture]")
{
    constexpr uint64_t mib = 1024 * 1024;
    MeshUploadQueue queue({ 32 * mib, 4 * mib });
    const TextureHandle texture{ 3, 1 };
    queue.Push({ {}, 64 * mib, 64 * mib, texture, 0, 0, 4096, 4096 * 4 });
    std::vector<bool> copied(4096);
    uint64_t bytes = 0;
    uint64_t frame = 1;
    while (queue.Pending() != 0 && frame < 100)
    {
        queue.CompleteUploads(frame > 2 ? frame - 2 : 0);
        const auto admitted = queue.Admit(frame++);
        REQUIRE_FALSE(admitted.empty());
        CHECK(queue.LastFrameBytes() <= 4 * mib);
        CHECK(queue.StagingUsed() <= 32 * mib);
        for (const auto &band : admitted)
        {
            REQUIRE(band.stagingOffset);
            CHECK(*band.stagingOffset + band.item.stagingBytes <= 32 * mib);
            for (uint32_t row = band.item.firstRow; row < band.item.firstRow + band.item.rowCount; ++row)
            {
                REQUIRE(row < copied.size());
                CHECK_FALSE(copied[row]);
                copied[row] = true;
            }
            bytes += band.item.stagingBytes;
        }
    }
    CHECK(bytes == 64 * mib);
    CHECK(queue.Pending() == 0);
    queue.CompleteUploads(frame);
    CHECK(queue.StagingUsed() == 0);
    CHECK(queue.UploadingBytes() == 0);
}

TEST_CASE("Partial texture bands rotate behind waiting meshes and cancellation keeps recorded spans alive",
    "[renderer][streaming][texture]")
{
    MeshUploadQueue queue({ 2048, 512 });
    const TextureHandle texture{ 4, 1 };
    queue.Push({ {}, 2048, 2048, texture, 0, 0, 8, 256 });
    queue.Push({ Mesh(9), 256, 256 });
    const auto first = queue.Admit(1);
    REQUIRE(first.size() == 1);
    CHECK(first[0].item.texture == texture);
    const auto second = queue.Admit(2);
    REQUIRE(second.size() == 2);
    CHECK(second[0].item.mesh == Mesh(9));
    CHECK(second[1].item.texture == texture);
    CHECK(queue.Cancel(texture));
    CHECK(queue.PendingBytes() == 0);
    CHECK(queue.UploadingBytes() == 1024);
    CHECK(queue.StagingUsed() != 0);
    queue.CompleteUploads(2);
    CHECK(queue.StagingUsed() == 0);
}

TEST_CASE("A texture row exceeding the budget still makes progress without exceeding the ring",
    "[renderer][streaming][texture]")
{
    MeshUploadQueue queue({ 512, 64 });
    queue.Push({ {}, 512, 512, TextureHandle{ 1, 1 }, 2, 0, 2, 256 });
    const auto admitted = queue.Admit(1);
    REQUIRE(admitted.size() == 1);
    CHECK(admitted[0].item.rowCount == 1);
    CHECK(admitted[0].item.mip == 2);
    CHECK(admitted[0].item.stagingBytes == 256);
    CHECK(queue.LastFrameBytes() == 256);
}

TEST_CASE("Prepared texture mips filter RGB in linear light and alpha linearly", "[renderer][texture]")
{
    const std::vector<std::byte> pixels(4, std::byte{ 0 });
    std::vector<std::byte> image = pixels;
    image.insert(image.end(), 4, std::byte{ 255 });
    auto source = PrepareTextureUpload({ .width = 2, .height = 1, .pixels = image, .srgb = true, .generateMips = true });
    REQUIRE(source.keepalive);
    REQUIRE(source.desc.mips.size() == 1);
    CHECK(std::to_integer<int>(source.desc.mips[0].pixels[0]) == 188);
    CHECK(std::to_integer<int>(source.desc.mips[0].pixels[3]) == 128);
    CHECK(source.Bytes() == 12);
    const std::vector<std::byte> supplied(4, std::byte{ 42 });
    TextureDesc desc{ .width = 2, .height = 1, .pixels = image, .generateMips = true };
    desc.mips.push_back({ 1, 1, supplied });
    source = PrepareTextureUpload(desc);
    REQUIRE(source.desc.mips.size() == 1);
    CHECK(source.desc.mips[0].pixels[0] == std::byte{ 42 });
    CHECK(source.desc.mips[0].pixels[3] == std::byte{ 42 });
    CHECK(PrepareTextureUpload({ .width = 2, .height = 2, .pixels = image }).desc.width == 0);
}

TEST_CASE("Compaction selection takes nothing when no BLAS is ready", "[renderer][streaming][compaction]")
{
    CHECK(SelectBlasCompactions({}, 1000) == 0);
}

TEST_CASE("Compaction selection takes every entry that fits the cap", "[renderer][streaming][compaction]")
{
    const std::vector<uint64_t> sizes{ 100, 200, 300 };
    CHECK(SelectBlasCompactions(sizes, 1000) == 3);
    CHECK(SelectBlasCompactions(sizes, 600) == 3);
}

TEST_CASE("Compaction selection stops before the entry that passes the cap", "[renderer][streaming][compaction]")
{
    const std::vector<uint64_t> sizes{ 400, 300, 300, 100 };
    CHECK(SelectBlasCompactions(sizes, 1000) == 3);
    CHECK(SelectBlasCompactions(sizes, 700) == 2);
    const std::vector<uint64_t> belowFirst{ 400, 300 };
    CHECK(SelectBlasCompactions(belowFirst, 399) == 1);
}

TEST_CASE("A single entry larger than the cap is still taken", "[renderer][streaming][compaction]")
{
    const std::vector<uint64_t> sizes{ 5000, 10 };
    CHECK(SelectBlasCompactions(sizes, 1000) == 1);
    const std::vector<uint64_t> single{ 5000 };
    CHECK(SelectBlasCompactions(single, 1000) == 1);
}
