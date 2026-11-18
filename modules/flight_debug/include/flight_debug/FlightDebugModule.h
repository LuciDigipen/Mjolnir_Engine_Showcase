/**
 * @file FlightDebugModule.h
 * @author Jackey Cen
 * @brief The Flight Debug window: a Tools drawer over the Play world's aircraft.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/Registration.h>
#include <core/ecs/Entity.h>
#include <core/ecs/ModuleContext.h>

namespace mir
{
    class ImGuiModule;
    class World;

    class FlightDebugModule
    {
    public:
        FlightDebugModule(ModuleContext context, ImGuiModule &imgui);
        ~FlightDebugModule();

        FlightDebugModule(const FlightDebugModule &) = delete;
        FlightDebugModule &operator=(const FlightDebugModule &) = delete;

        void ActivateWorld(World *world);

    private:
        void Draw();

        World *mAuthoringWorld;
        World *mActiveWorld;
        bool mPlayWorldActive = false;
        bool mVisible = false;
        Entity mSelectedAircraft = kNullEntity;

        // last: unregisters before the worlds and ImGui go
        Registration mDrawer;
    };
}
