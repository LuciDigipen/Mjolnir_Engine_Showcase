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

#include <string>
#include <vector>

namespace mir
{
    class ImGuiModule;
    class World;

    class FlightDebugModule
    {
    public:
        // The source assets root is where Save writes; a non-editor build passes it empty.
        FlightDebugModule(ModuleContext context, ImGuiModule &imgui, std::string sourceAssetsRoot = {});
        ~FlightDebugModule();

        FlightDebugModule(const FlightDebugModule &) = delete;
        FlightDebugModule &operator=(const FlightDebugModule &) = delete;

        void ActivateWorld(World *world);

    private:
        void Draw();
        void CollectOverwrittenEdits();

        World *mAuthoringWorld;
        World *mActiveWorld;
        std::string mSourceAssetsRoot;
        std::string mSaveError;
        std::vector<std::string> mOverwrittenEdits;
        bool mPlayWorldActive = false;
        bool mVisible = false;
        Entity mSelectedAircraft = kNullEntity;

        // last: unregisters before the worlds and ImGui go
        Registration mDrawer;
    };
}
