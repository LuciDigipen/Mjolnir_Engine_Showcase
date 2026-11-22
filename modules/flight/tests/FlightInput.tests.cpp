/**
 * @file FlightInput.tests.cpp
 * @author Jackey Cen
 * @brief Tests for flight/FlightInput.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <flight/FlightInput.h>

#include <input/InputMap.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <iterator>
#include <string>
#include <string_view>

namespace
{
    struct ExpectedAction
    {
        std::string_view name;
        mir::InputActionType type;
    };

    constexpr ExpectedAction kActions[] = {
        { mir::kFlightPitchAction, mir::InputActionType::Axis1D },
        { mir::kFlightRollAction, mir::InputActionType::Axis1D },
        { mir::kFlightYawAction, mir::InputActionType::Axis1D },
        { mir::kFlightThrottleUpAction, mir::InputActionType::Button },
        { mir::kFlightThrottleDownAction, mir::InputActionType::Button },
        { mir::kFlightBrakeAction, mir::InputActionType::Button },
        { mir::kFlightFlapsUpAction, mir::InputActionType::Button },
        { mir::kFlightFlapsDownAction, mir::InputActionType::Button },
        { mir::kFlightTrimUpAction, mir::InputActionType::Button },
        { mir::kFlightTrimDownAction, mir::InputActionType::Button },
        { mir::kFlightCameraToggleAction, mir::InputActionType::Button },
        { mir::kFlightDebugToggleAction, mir::InputActionType::Button },
    };

    bool BindsDevice(const mir::InputAction &action, mir::DeviceKind device)
    {
        return std::any_of(action.bindings.begin(), action.bindings.end(),
            [device](const mir::InputBinding &binding) { return binding.device == device; });
    }

    const mir::InputBinding *FindPadAxis(const mir::InputAction &action, mir::GamepadAxis axis)
    {
        const auto found = std::find_if(action.bindings.begin(), action.bindings.end(),
            [axis](const mir::InputBinding &binding)
            { return binding.device == mir::DeviceKind::Gamepad && binding.useGamepadAxis && binding.gamepadAxis == axis; });
        return found == action.bindings.end() ? nullptr : &*found;
    }
}

TEST_CASE("Every flight action exists with its type", "[flight][input]")
{
    mir::InputMap map;
    CHECK(mir::AddFlightDefaultActions(map) == std::size(kActions));

    for (const ExpectedAction &expected : kActions)
    {
        INFO(expected.name);
        const mir::InputAction *action = map.Find(expected.name);
        REQUIRE(action != nullptr);
        CHECK(action->type == expected.type);
    }
}

TEST_CASE("Flight actions bind a keyboard and the controls a gamepad", "[flight][input]")
{
    mir::InputMap map;
    mir::AddFlightDefaultActions(map);

    for (const ExpectedAction &expected : kActions)
    {
        INFO(expected.name);
        const mir::InputAction &action = *map.Find(expected.name);
        CHECK(BindsDevice(action, mir::DeviceKind::Keyboard));
        if (expected.name != mir::kFlightDebugToggleAction)
            CHECK(BindsDevice(action, mir::DeviceKind::Gamepad));
    }

    const mir::InputAction &yaw = *map.Find(mir::kFlightYawAction);
    const mir::InputBinding *right = FindPadAxis(yaw, mir::GamepadAxis::RightTrigger);
    const mir::InputBinding *left = FindPadAxis(yaw, mir::GamepadAxis::LeftTrigger);
    REQUIRE(right != nullptr);
    REQUIRE(left != nullptr);
    CHECK(right->scale == 1.0f);
    CHECK(left->scale == -1.0f);

    const mir::InputAction &debug = *map.Find(mir::kFlightDebugToggleAction);
    REQUIRE(debug.bindings.size() == 1);
    CHECK(debug.bindings[0].device == mir::DeviceKind::Keyboard);
    CHECK(debug.bindings[0].key == mir::Key::F3);
}

TEST_CASE("A second call adds nothing", "[flight][input]")
{
    mir::InputMap map;
    REQUIRE(mir::AddFlightDefaultActions(map) == std::size(kActions));

    CHECK(mir::AddFlightDefaultActions(map) == 0);
    CHECK(map.Actions().size() == std::size(kActions));
}

TEST_CASE("A rebound flight action is kept", "[flight][input]")
{
    mir::InputMap map;
    map.AddAction(std::string(mir::kFlightPitchAction), mir::InputActionType::Axis1D)
        .bindings.push_back(mir::InputBinding{ .device = mir::DeviceKind::Keyboard, .key = mir::Key::I });

    CHECK(mir::AddFlightDefaultActions(map) == std::size(kActions) - 1);

    const mir::InputAction &pitch = *map.Find(mir::kFlightPitchAction);
    REQUIRE(pitch.bindings.size() == 1);
    CHECK(pitch.bindings[0].key == mir::Key::I);
}
