/**
 * @file TextureTable.tests.cpp
 * @author Sumin Park
 * @brief Tests for renderer/TextureTable.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer/TextureTable.h>

#include <catch2/catch_test_macros.hpp>

using namespace mir;

TEST_CASE("Texture slots stay stable and reuse advances the generation", "[renderer][material]")
{
    TextureTable table(4);

    const TextureHandle first = table.Add();
    const TextureHandle second = table.Add();
    REQUIRE_FALSE(first.IsNull());
    REQUIRE_FALSE(second.IsNull());
    CHECK(first.mIndex == 0);
    CHECK(second.mIndex == 1);
    CHECK(table.Contains(first));
    CHECK(table.Contains(second));

    REQUIRE(table.Remove(first));
    CHECK_FALSE(table.Contains(first));
    CHECK_FALSE(table.Remove(first));

    const TextureHandle reused = table.Add();
    REQUIRE_FALSE(reused.IsNull());
    CHECK(reused.mIndex == first.mIndex);
    CHECK(reused.mGeneration != first.mGeneration);
    CHECK_FALSE(table.Contains(first));
    CHECK(table.Contains(reused));
}

TEST_CASE("A full texture table refuses new rows and ignores unknown handles", "[renderer][material]")
{
    TextureTable table(1);
    const TextureHandle first = table.Add();
    REQUIRE_FALSE(first.IsNull());
    CHECK(table.Add().IsNull());
    CHECK_FALSE(table.Contains(kNullTexture));
    CHECK_FALSE(table.Remove(kNullTexture));
    CHECK_FALSE(table.Contains(TextureHandle{ 99, 1 }));
    CHECK(table.Generation(99) == 0);
}
