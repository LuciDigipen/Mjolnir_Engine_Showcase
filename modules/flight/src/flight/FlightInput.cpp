/**
 * @file FlightInput.cpp
 * @author Jackey Cen
 * @brief The flight actions and their default keyboard and gamepad bindings.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <flight/FlightInput.h>

#include <string>

namespace mir
{
    namespace
    {
        InputBinding KeyBinding(Key key, float scale = 1.0f)
        {
            return InputBinding{ .device = DeviceKind::Keyboard, .key = key, .scale = scale };
        }

        InputBinding PadButtonBinding(GamepadButton button)
        {
            return InputBinding{ .device = DeviceKind::Gamepad, .gamepadButton = button };
        }

        InputBinding PadAxisBinding(GamepadAxis axis, float scale = 1.0f)
        {
            return InputBinding{ .device = DeviceKind::Gamepad, .useGamepadAxis = true, .gamepadAxis = axis,
                .scale = scale };
        }

        InputMap FlightDefaultMap()
        {
            InputMap map;

            map.AddAction(std::string(kFlightPitchAction), InputActionType::Axis1D).bindings = {
                PadAxisBinding(GamepadAxis::LeftY),
                KeyBinding(Key::S, 1.0f),
                KeyBinding(Key::W, -1.0f),
            };

            map.AddAction(std::string(kFlightRollAction), InputActionType::Axis1D).bindings = {
                PadAxisBinding(GamepadAxis::LeftX),
                KeyBinding(Key::D, 1.0f),
                KeyBinding(Key::A, -1.0f),
            };

            map.AddAction(std::string(kFlightYawAction), InputActionType::Axis1D).bindings = {
                PadAxisBinding(GamepadAxis::RightTrigger, 1.0f),
                PadAxisBinding(GamepadAxis::LeftTrigger, -1.0f),
                KeyBinding(Key::E, 1.0f),
                KeyBinding(Key::Q, -1.0f),
            };

            map.AddAction(std::string(kFlightThrottleUpAction), InputActionType::Button).bindings = {
                PadButtonBinding(GamepadButton::A),
                KeyBinding(Key::R),
            };

            map.AddAction(std::string(kFlightThrottleDownAction), InputActionType::Button).bindings = {
                PadButtonBinding(GamepadButton::B),
                KeyBinding(Key::F),
            };

            map.AddAction(std::string(kFlightBrakeAction), InputActionType::Button).bindings = {
                PadButtonBinding(GamepadButton::X),
                KeyBinding(Key::B),
            };

            map.AddAction(std::string(kFlightFlapsUpAction), InputActionType::Button).bindings = {
                PadButtonBinding(GamepadButton::DpadUp),
                KeyBinding(Key::LeftBracket),
            };

            map.AddAction(std::string(kFlightFlapsDownAction), InputActionType::Button).bindings = {
                PadButtonBinding(GamepadButton::DpadDown),
                KeyBinding(Key::RightBracket),
            };

            map.AddAction(std::string(kFlightTrimUpAction), InputActionType::Button).bindings = {
                PadButtonBinding(GamepadButton::DpadLeft),
                KeyBinding(Key::T),
            };

            map.AddAction(std::string(kFlightTrimDownAction), InputActionType::Button).bindings = {
                PadButtonBinding(GamepadButton::DpadRight),
                KeyBinding(Key::G),
            };

            map.AddAction(std::string(kFlightCameraToggleAction), InputActionType::Button).bindings = {
                PadButtonBinding(GamepadButton::Y),
                KeyBinding(Key::V),
            };

            map.AddAction(std::string(kFlightDebugToggleAction), InputActionType::Button).bindings = {
                KeyBinding(Key::F3),
            };

            return map;
        }
    }

    std::size_t AddFlightDefaultActions(InputMap &map)
    {
        const InputMap defaults = FlightDefaultMap();

        std::size_t added = 0;
        for (const InputAction &action : defaults.Actions())
        {
            if (map.Find(action.name) != nullptr)
                continue;

            map.MutableActions().push_back(action);
            ++added;
        }
        return added;
    }
}
