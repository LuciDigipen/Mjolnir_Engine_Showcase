/**
 * @file EditorCamera.tests.cpp
 * @author Quy Dong Ta
 * @brief Tests for editor/EditorCamera: a jump marks a camera cut, an ordinary move does not.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <editor/EditorCamera.h>

#include <catch2/catch_test_macros.hpp>

#include <optional>

namespace
{
    mir::WorldPosition At(const mir::WorldVec3 &meters)
    {
        const std::optional<mir::WorldPosition> position = mir::WorldPosition::FromMeters(meters);
        REQUIRE(position);
        return *position;
    }
}

TEST_CASE("Focusing the editor camera cuts it once", "[editor][camera]")
{
    mir::EditorCamera camera;
    CHECK_FALSE(camera.TakeCut());

    REQUIRE(camera.FocusOn(At({ 40.0, 0.0, -30.0 }), 1.0f));
    CHECK(camera.TakeCut());
    CHECK_FALSE(camera.TakeCut());
}

TEST_CASE("Placing the editor camera cuts it", "[editor][camera]")
{
    mir::EditorCamera camera;
    camera.SetPosition(At({ 500.0, 10.0, 500.0 }));
    CHECK(camera.TakeCut());
}

TEST_CASE("Flying the editor camera does not cut it", "[editor][camera]")
{
    mir::EditorCamera camera;
    REQUIRE(camera.Translate({ 0.1, 0.0, 0.0 }));
    CHECK_FALSE(camera.TakeCut());
}
