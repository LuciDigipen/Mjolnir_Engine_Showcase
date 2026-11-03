/**
 * @file GameUIImGuiModule.h
 * @author sumin.park
 * @brief Draws the game UI through ImGui: installs the backend and runs gameui's frame in the Game drawer group.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <gameui_imgui/ImGuiUIBackend.h>

#include <core/ecs/ModuleContext.h>
#include <core/Registration.h>

namespace mir
{
    class GameUIModule;
    class ImGuiModule;
    class WindowModule;

    // A bridge: gameui never sees ImGui, and imguiruntime never sees gameui. The game
    // UI itself is declared before ImGuiModule, so its textures outlive ImGui's
    // shutdown, which frees their GPU side
    class GameUIImGuiModule
    {
    public:
        GameUIImGuiModule(ModuleContext context, ImGuiModule &imgui, WindowModule &window, GameUIModule &gameUI);
        ~GameUIImGuiModule();

        GameUIImGuiModule(const GameUIImGuiModule &) = delete;
        GameUIImGuiModule &operator=(const GameUIImGuiModule &) = delete;

        void ActivateWorld(World *world, GameUIModule *gameUI);

    private:
        World *mAuthoringWorld;
        World *mActiveWorld;
        GameUIModule &mAuthoringGameUI;
        GameUIModule *mActiveGameUI;
        ImGuiUIBackend mBackend;

        // last: unregisters before the backend above is destroyed
        Registration mDrawer;
    };
}
