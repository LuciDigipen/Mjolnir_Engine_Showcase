/**
 * @file ControlsSettings.tests.cpp
 * @author Quy Dong Ta
 * @brief Tests for input/ControlsSettings: inverted mouse Y and inverted
 *        gamepad Y applied by InputSystem to the camera's look and nothing
 *        else, the per-user save and load beside the audio section, and
 *        InputModule applying and saving a change.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <input/ControlsSettings.h>
#include <input/DefaultInputMap.h>
#include <input/InputMap.h>
#include <input/InputModule.h>
#include <input/InputState.h>
#include <input/InputSystem.h>

#include <core/ecs/CommandBuffer.h>
#include <core/ecs/SystemScheduler.h>
#include <core/ecs/World.h>
#include <core/fs/FileIO.h>
#include <window/Window.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <optional>
#include <string>

using namespace mir;

namespace
{
    class TestWindow final : public Window
    {
    public:
        void PollEvents() override {}
        bool ShouldClose() const override { return false; }
        NativeWindowHandle NativeWindow() const override { return {}; }
        uint32_t Width() const override { return 1; }
        uint32_t Height() const override { return 1; }
        const RawInputSnapshot &RawInput() const override { return mRaw; }

        RawInputSnapshot mRaw;
    };

    struct Fixture
    {
        TestWindow window;
        World world;
        CommandBuffer commands;
        InputMap &map = world.EmplaceResource<InputMap>(DefaultInputMap());
        InputState &state = world.EmplaceResource<InputState>();
        ControlsSettings &controls = world.EmplaceResource<ControlsSettings>();
        InputSystem system{window};
        SystemContext context{world, commands, 0.016f, 0.0, 0};

        void Update() { system.OnUpdate(context); }

        // the mouse moved by (dx, dy) since the last update
        void MoveMouse(double dx, double dy)
        {
            Update();
            window.mRaw.mouseX += dx;
            window.mRaw.mouseY += dy;
            Update();
        }

        void Stick(GamepadAxis axis, float value)
        {
            window.mRaw.gamepads[0].connected = true;
            window.mRaw.gamepads[0].axes[static_cast<size_t>(axis)] = value;
            Update();
        }
    };

    struct TempSettingsFile
    {
        std::filesystem::path mPath;

        explicit TempSettingsFile(const char *name)
            : mPath(std::filesystem::temp_directory_path() / "mir_input_tests" / name)
        {
            std::filesystem::create_directories(mPath.parent_path());
            std::filesystem::remove(mPath);
        }
        ~TempSettingsFile() { std::filesystem::remove(mPath); }
    };
}

TEST_CASE("Nothing is inverted by default", "[input][controls]")
{
    Fixture f;
    f.MoveMouse(4.0, 6.0);
    CHECK(f.state.Get(kCameraLookAction).x == 4.0f);
    CHECK(f.state.Get(kCameraLookAction).y == 6.0f);

    f.Stick(GamepadAxis::RightY, 0.5f);
    CHECK(f.state.Get(kCameraLookStickAction).y == 0.5f);
}

TEST_CASE("Invert mouse Y flips the mouse's vertical look alone", "[input][controls]")
{
    Fixture f;
    f.controls.mInvertMouseY = true;

    f.MoveMouse(4.0, 6.0);
    CHECK(f.state.Get(kCameraLookAction).x == 4.0f);
    CHECK(f.state.Get(kCameraLookAction).y == -6.0f);

    // the stick is another device's setting
    f.Stick(GamepadAxis::RightY, 0.5f);
    CHECK(f.state.Get(kCameraLookStickAction).y == 0.5f);
}

TEST_CASE("Invert gamepad Y flips the right stick's vertical axis, never the left stick's", "[input][controls]")
{
    Fixture f;
    f.controls.mInvertGamepadY = true;

    f.Stick(GamepadAxis::RightY, 0.5f);
    f.Stick(GamepadAxis::RightX, 0.25f);
    CHECK(f.state.Get(kCameraLookStickAction).y == -0.5f);
    CHECK(f.state.Get(kCameraLookStickAction).x == 0.25f);

    // the left stick moves the camera and navigates menus, so up stays up
    f.Stick(GamepadAxis::LeftY, 0.75f);
    CHECK(f.state.Get(kCameraMoveAction).y == 0.75f);
    CHECK(f.state.Get(kUINavigateAction).y == 0.75f);

    f.MoveMouse(0.0, 6.0);
    CHECK(f.state.Get(kCameraLookAction).y == 6.0f);
}

TEST_CASE("Controls settings round-trip and keep the file's other sections", "[input][controls]")
{
    TempSettingsFile file("controls_roundtrip.json");
    REQUIRE(WriteText(file.mPath, R"({ "audio": { "music": { "volume": 0.3, "muted": true } } })"));

    const ControlsSettings saved{.mInvertMouseY = true, .mInvertGamepadY = false};
    REQUIRE(SaveControlsSettings(file.mPath, saved));
    CHECK(LoadControlsSettings(file.mPath) == saved);

    const std::optional<std::string> text = ReadText(file.mPath);
    REQUIRE(text.has_value());
    CHECK(text->find("\"audio\"") != std::string::npos);
    CHECK(text->find("\"muted\": true") != std::string::npos);
}

TEST_CASE("A missing file, a broken file or a bad value falls back to the defaults", "[input][controls]")
{
    TempSettingsFile file("controls_bad.json");
    CHECK(LoadControlsSettings(file.mPath) == ControlsSettings{});

    REQUIRE(WriteText(file.mPath, "{ not json"));
    CHECK(LoadControlsSettings(file.mPath) == ControlsSettings{});

    REQUIRE(WriteText(file.mPath, R"({ "controls": { "invertMouseY": "yes", "invertGamepadY": true } })"));
    CHECK(LoadControlsSettings(file.mPath) == ControlsSettings{.mInvertMouseY = false, .mInvertGamepadY = true});
}

TEST_CASE("InputModule loads the player's controls, applies a change and saves it for the next run",
          "[input][controls]")
{
    TempSettingsFile file("controls_module.json");
    TempSettingsFile project("controls_module_project.json");
    REQUIRE(SaveControlsSettings(file.mPath, ControlsSettings{.mInvertGamepadY = true}));

    TestWindow window;
    World world;
    SystemScheduler scheduler;
    InputModule input({world, scheduler}, window, project.mPath, file.mPath);
    CHECK(input.Controls().mInvertGamepadY);
    CHECK(world.Resource<ControlsSettings>().mInvertGamepadY);

    input.SetControls(ControlsSettings{.mInvertMouseY = true});
    CHECK(world.Resource<ControlsSettings>() == ControlsSettings{.mInvertMouseY = true});
    CHECK(LoadControlsSettings(file.mPath) == ControlsSettings{.mInvertMouseY = true});
}
