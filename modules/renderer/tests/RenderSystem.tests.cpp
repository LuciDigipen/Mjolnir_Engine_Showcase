/**
 * @file RenderSystem.tests.cpp
 * @author Jackey Cen
 * @brief Tests for renderer/RenderSystem.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer/RenderSystem.h>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("A source World without a Sun or a Sky gets default lighting", "[renderer]")
{
    const mir::SourceLighting lighting = mir::ResolveSourceLighting(false, false, true);
    CHECK(lighting.mUseDefaultSun);
    CHECK(lighting.mUseDefaultSky);
}

TEST_CASE("Source lighting defaults never replace authored lights", "[renderer]")
{
    const mir::SourceLighting sun = mir::ResolveSourceLighting(true, false, true);
    CHECK_FALSE(sun.mUseDefaultSun);
    CHECK_FALSE(sun.mUseDefaultSky);

    const mir::SourceLighting sky = mir::ResolveSourceLighting(false, true, true);
    CHECK_FALSE(sky.mUseDefaultSun);
    CHECK_FALSE(sky.mUseDefaultSky);
}

TEST_CASE("The main World keeps its own lighting", "[renderer]")
{
    const mir::SourceLighting lighting = mir::ResolveSourceLighting(false, false, false);
    CHECK_FALSE(lighting.mUseDefaultSun);
    CHECK_FALSE(lighting.mUseDefaultSky);
}
