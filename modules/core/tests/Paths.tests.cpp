/**
 * @file Paths.tests.cpp
 * @author Sumin Park
 * @brief Tests for core/fs/Paths.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <core/fs/Paths.h>

#include <catch2/catch_test_macros.hpp>

#include <string>

// fails on Windows when the executable is missing engine_target_utf8
TEST_CASE("A path narrows to UTF-8 and back, whatever the system code page", "[paths]")
{
    // CJK and Latin-1 letters: no ANSI code page holds both
    const std::filesystem::path path(u8"地形-ü");
    const std::u8string utf8 = path.u8string();
    const std::string expected(utf8.begin(), utf8.end());

    std::string narrow;
    REQUIRE_NOTHROW(narrow = path.string());
    CHECK(narrow == expected);
    CHECK(std::filesystem::path(narrow) == path);
}

TEST_CASE("ExecutableDir points at a real directory", "[paths]")
{
    REQUIRE(std::filesystem::exists(mir::ExecutableDir()));
    REQUIRE(std::filesystem::is_directory(mir::ExecutableDir()));
}

TEST_CASE("ShaderPath appends shaders/<name> under the executable directory", "[paths]")
{
    const auto path = mir::ShaderPath("triangle.spv");
    REQUIRE(path == mir::ExecutableDir() / "shaders" / "triangle.spv");
}

TEST_CASE("FontPath appends fonts/<name> under the executable directory", "[paths]")
{
    const auto path = mir::FontPath("Inter.ttf");
    REQUIRE(path == mir::ExecutableDir() / "fonts" / "Inter.ttf");
}

TEST_CASE("CookedAssetsDir points at cooked/ under the executable directory", "[paths]")
{
    const auto path = mir::CookedAssetsDir();
    REQUIRE(path == mir::ExecutableDir() / "cooked");
}

TEST_CASE("LogDir points at logs/ under the executable directory", "[paths]")
{
    REQUIRE(mir::LogDir() == mir::ExecutableDir() / "logs");
}
