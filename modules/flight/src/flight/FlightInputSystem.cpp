/**
 * @file FlightInputSystem.cpp
 * @author Jackey Cen
 * @brief Reads the flight actions into each player aircraft and queues its discrete commands.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <flight/FlightInputSystem.h>

#include <flight/FlightInput.h>
#include <flight/components/Aircraft.h>

#include <core/ecs/EngineMode.h>
#include <core/ecs/World.h>
#include <input/InputState.h>

namespace mir
{
    void FlightInputSystem::OnStart(SystemContext &context)
    {
        mQuery = &context.world.GetOrCreateQuery<Aircraft>();
    }

    void FlightInputSystem::OnUpdate(SystemContext &context)
    {
        if (!IsSimulating(context.world))
            return;

        const InputState *input = context.world.TryResource<InputState>();
        if (input == nullptr)
            return;

        mQuery->ForEach(
            [input](Entity, Aircraft &aircraft)
            {
                if (!aircraft.playerInput)
                    return;

                FlightControls &controls = aircraft.runtime.controls;
                controls.pitch = input->ForGame(kFlightPitchAction).x;
                controls.roll = input->ForGame(kFlightRollAction).x;
                controls.yaw = input->ForGame(kFlightYawAction).x;
                controls.throttleUp = input->ForGame(kFlightThrottleUpAction).down;
                controls.throttleDown = input->ForGame(kFlightThrottleDownAction).down;
                controls.brake = input->ForGame(kFlightBrakeAction).down;

                FlightCommandQueue &commands = aircraft.runtime.commands;
                if (input->ForGame(kFlightFlapsUpAction).justPressed)
                    commands.Push(FlightCommand::FlapsUp);
                if (input->ForGame(kFlightFlapsDownAction).justPressed)
                    commands.Push(FlightCommand::FlapsDown);
                if (input->ForGame(kFlightTrimUpAction).justPressed)
                    commands.Push(FlightCommand::TrimUp);
                if (input->ForGame(kFlightTrimDownAction).justPressed)
                    commands.Push(FlightCommand::TrimDown);
            });
    }
}
