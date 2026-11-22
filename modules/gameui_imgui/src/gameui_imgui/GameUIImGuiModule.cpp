/**
 * @file GameUIImGuiModule.cpp
 * @author sumin.park
 * @brief Draws the game UI through ImGui: installs the backend and runs gameui's frame in the Game drawer group.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <gameui_imgui/GameUIImGuiModule.h>

#include <core/ecs/World.h>
#include <gameui/GameUIArea.h>
#include <gameui/GameUIModule.h>
#include <imguiruntime/ImGuiModule.h>
#include <window/WindowModule.h>

#include <imgui.h>

namespace mir
{
    GameUIImGuiModule::GameUIImGuiModule(ModuleContext context, ImGuiModule &imgui, WindowModule &window,
        GameUIModule &gameUI)
        : mAuthoringWorld(&context.world), mActiveWorld(&context.world), mAuthoringGameUI(gameUI), mActiveGameUI(&gameUI)
    {
        gameUI.Context().SetBackend(&mBackend);

        // the Game group runs after Tools, so the editor has written this frame's area
        mDrawer = imgui.AddDrawer(DrawerGroup::Game, [this, &window]
            {
                ImGuiUIBackend::RetireTextures();

                const Window &surface = window.Get();
                if (surface.Width() == 0 || surface.Height() == 0)
                    return;

                const GameUIArea &published = mActiveWorld->Resource<GameUIArea>();
                const UIRect area = published.mSet ? published.mRect
                                                   : UIRect{ 0.0f, 0.0f, static_cast<float>(surface.Width()),
                                                             static_cast<float>(surface.Height()) };
                const RawInputSnapshot &input = surface.RawInput();
                // an ImGui window, popup or drag, or an editor tool over the viewport, is above the game UI
                const bool withheld = ImGui::GetIO().WantCaptureMouse || published.mToolsHavePointer;
                mActiveGameUI->RunFrame(area, UIPointer{ static_cast<float>(input.mouseX), static_cast<float>(input.mouseY),
                                                  input.IsDown(MouseButton::Left), withheld });

                // ImGui's GLFW backend sets the OS cursor from this every frame, so it is the only place to hide it
                if (!withheld && !mActiveGameUI->Suspended() && mActiveGameUI->Context().HidesCursor())
                    ImGui::SetMouseCursor(ImGuiMouseCursor_None);
            });
    }

    void GameUIImGuiModule::ActivateWorld(World *world, GameUIModule *gameUI)
    {
        mActiveGameUI->Context().SetBackend(nullptr);
        mActiveWorld = world != nullptr ? world : mAuthoringWorld;
        mActiveGameUI = gameUI != nullptr ? gameUI : &mAuthoringGameUI;
        mActiveGameUI->Context().SetBackend(&mBackend);
    }

    // stops new copies; the made ones release themselves when the game UI goes
    GameUIImGuiModule::~GameUIImGuiModule()
    {
        mDrawer.Reset();
        mActiveGameUI->Context().SetBackend(nullptr);
    }
}
