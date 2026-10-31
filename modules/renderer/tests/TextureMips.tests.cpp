/**
 * @file TextureMips.tests.cpp
 * @author Jackey Cen
 * @brief Tests for renderer/Texture.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer/Texture.h>

#include <catch2/catch_test_macros.hpp>

using namespace mir;

namespace
{
    std::byte Pixel(std::uint8_t value) { return static_cast<std::byte>(value); }
} // namespace

TEST_CASE("A supplied mip chain must halve every level down to 1x1", "[renderer][texture]")
{
    const std::vector<std::byte> level0(4 * 4 * 4, Pixel(255));
    const std::vector<std::byte> level1(2 * 2 * 4, Pixel(128));
    const std::vector<std::byte> level2(1 * 1 * 4, Pixel(64));
    TextureDesc desc{ .width = 4, .height = 4, .pixels = level0 };
    CHECK_FALSE(TextureMipLayoutValid(desc));

    desc.mips = {{2, 2, level1}, {1, 1, level2}};
    CHECK(TextureMipLayoutValid(desc));
}

TEST_CASE("A supplied mip chain rejects wrong extents and byte counts", "[renderer][texture]")
{
    const std::vector<std::byte> level0(8 * 4 * 4, Pixel(255));
    const std::vector<std::byte> level1(4 * 2 * 4, Pixel(128));
    const std::vector<std::byte> level2(2 * 1 * 4, Pixel(64));
    const std::vector<std::byte> level3(1 * 1 * 4, Pixel(32));

    TextureDesc desc{ .width = 8, .height = 4, .pixels = level0 };
    desc.mips = {{4, 2, level1}, {2, 1, level2}, {1, 1, level3}};
    CHECK(TextureMipLayoutValid(desc));

    desc.mips[1].width = 3;
    CHECK_FALSE(TextureMipLayoutValid(desc));

    desc.mips[1].width = 2;
    desc.mips[2].pixels = std::span<const std::byte>(level3).subspan(1);
    CHECK_FALSE(TextureMipLayoutValid(desc));

    desc.mips.pop_back();
    CHECK_FALSE(TextureMipLayoutValid(desc));
}
