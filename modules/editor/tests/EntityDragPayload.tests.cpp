#include <editor/EntityDragPayload.h>
#include <core/ecs/World.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstring>

TEST_CASE("Entity drag payload preserves the source row handle", "[editor][entity-ref]")
{
    mir::World world;
    const mir::Entity source = world.CreateEntity();
    const uint64_t packed = mir::PackEntity(source);
    // Deliberately unaligned: payload storage need not be a uint64_t pointer.
    std::array<char, sizeof(packed) + 1> storage{};
    std::memcpy(storage.data() + 1, &packed, sizeof(packed));
    ImGuiPayload payload;
    payload.DataFrameCount = 0;
    std::strcpy(payload.DataType, mir::kEntityPayload);
    payload.Data = storage.data() + 1;
    payload.DataSize = sizeof(packed);
    REQUIRE(mir::DecodeEntityDragPayload(payload) == source);
    world.DestroyEntity(source);
    const mir::Entity replacement = world.CreateEntity();
    REQUIRE(replacement.mIndex == source.mIndex);
    REQUIRE(replacement.mGeneration != source.mGeneration);
    const auto decoded = mir::DecodeEntityDragPayload(payload);
    REQUIRE(decoded.has_value());
    CHECK_FALSE(world.IsAlive(*decoded));
    CHECK(*decoded != replacement);
}

TEST_CASE("Entity drag payload rejects invalid data", "[editor][entity-ref]")
{
    uint64_t packed = mir::PackEntity(mir::Entity{ 3, 7 });
    ImGuiPayload payload;
    payload.DataFrameCount = 0;
    std::strcpy(payload.DataType, mir::kEntityPayload);
    payload.Data = &packed;
    payload.DataSize = sizeof(packed);
    SECTION("wrong type") { std::strcpy(payload.DataType, "MIR_PREFAB_CONTENT"); }
    SECTION("missing data") { payload.Data = nullptr; }
    SECTION("empty") { payload.DataSize = 0; }
    SECTION("truncated") { --payload.DataSize; }
    SECTION("oversized") { ++payload.DataSize; }
    SECTION("null entity") { packed = mir::PackEntity(mir::kNullEntity); }
    CHECK_FALSE(mir::DecodeEntityDragPayload(payload).has_value());
}
