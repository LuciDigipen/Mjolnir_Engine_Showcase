/**
 * @file ComponentJson.tests.cpp
 * @author Jackey Cen
 * @brief Tests for scene/ComponentJson.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <scene/ComponentJson.h>

#include "core/math/WorldReal.h"

#include <catch2/catch_test_macros.hpp>

#include <glm/vec3.hpp>

#include <cstddef>
#include <cstring>
#include <string>

namespace
{
    using nlohmann::json;
    using mir::FieldKind;
}

TEST_CASE("JsonToField rejects mismatched values without writing", "[scene][json]")
{
    alignas(16) std::byte buffer[64];

    const auto expectRejected = [&buffer](FieldKind kind, const json &value)
        {
            std::memset(buffer, 0x5A, sizeof(buffer));
            CHECK_FALSE(mir::JsonToField(kind, value, buffer));
            for (std::byte byte : buffer)
                CHECK(byte == std::byte{ 0x5A });
        };

    expectRejected(FieldKind::Bool, json(1));
    expectRejected(FieldKind::Int, json("7"));
    expectRejected(FieldKind::Int, json(3.5));
    expectRejected(FieldKind::Float, json("1.0"));
    expectRejected(FieldKind::Vec3, json::array({ 1, 2 }));
    expectRejected(FieldKind::Vec3, json::array({ 1, 2, "3" }));
    expectRejected(FieldKind::Vec4, json::array({ 1, 2, 3 }));
    expectRejected(FieldKind::Quat, json::array({ 1, 2, 3 }));
    expectRejected(FieldKind::Mat4, json::array({ 1 }));
    expectRejected(FieldKind::Handle, json::array({ 1 }));
    expectRejected(FieldKind::Handle, json::array({ -1, 2 }));
    expectRejected(FieldKind::String, json(5));
    expectRejected(FieldKind::WorldPosition, json::array({ 1, 2 }));
    expectRejected(FieldKind::WorldPosition, json::array({ 1, 2, "3" }));
}

TEST_CASE("JsonToField decodes the field kinds scene data uses", "[scene][json]")
{
    alignas(16) std::byte buffer[64];

    REQUIRE(mir::JsonToField(FieldKind::Vec3, json::array({ 1.5, -2.0, 3.0 }), buffer));
    CHECK(*reinterpret_cast<const glm::vec3 *>(buffer) == glm::vec3(1.5f, -2.0f, 3.0f));

    REQUIRE(mir::JsonToField(FieldKind::Float, json(2), buffer));
    CHECK(*reinterpret_cast<const float *>(buffer) == 2.0f);

    const std::string longText(mir::kFieldStringCapacity + 10, 'x');
    REQUIRE(mir::JsonToField(FieldKind::String, json(longText), buffer));
    const char *text = reinterpret_cast<const char *>(buffer);
    CHECK(std::strlen(text) == mir::kFieldStringCapacity - 1);
    CHECK(text[mir::kFieldStringCapacity - 1] == '\0');

    REQUIRE(mir::JsonToField(FieldKind::WorldPosition, json::array({ 1.0, 2.0, 3.0 }), buffer));
    CHECK(*reinterpret_cast<const mir::WorldVec3 *>(buffer) == mir::WorldVec3(1.0, 2.0, 3.0));
}

TEST_CASE("JsonToField leaves EntityRef values to the caller", "[scene][json]")
{
    alignas(16) std::byte buffer[64];
    std::memset(buffer, 0x5A, sizeof(buffer));

    CHECK(mir::JsonToField(FieldKind::EntityRef, json::array({ "2" }), buffer));
    for (std::byte byte : buffer)
        CHECK(byte == std::byte{ 0x5A });
}
