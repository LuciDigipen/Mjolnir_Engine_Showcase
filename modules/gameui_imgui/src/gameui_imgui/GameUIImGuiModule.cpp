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
        : mGameUI(gameUI)
    {
        gameUI.Context().SetBackend(&mBackend);

        // the Game group runs after Tools, so the editor has written this frame's area
        mDrawer = imgui.AddDrawer(DrawerGroup::Game, [this, &window, &world = context.world]
            {
                const Window &surface = window.Get();
                if (surface.Width() == 0 || surface.Height() == 0)
                    return;

                const GameUIArea &published = world.Resource<GameUIArea>();
                const UIRect area = published.mSet ? published.mRect
                                                   : UIRect{ 0.0f, 0.0f, static_cast<float>(surface.Width()),
                                                             static_cast<float>(surface.Height()) };
                const RawInputSnapshot &input = surface.RawInput();
                mGameUI.RunFrame(area, UIPointer{ static_cast<float>(input.mouseX), static_cast<float>(input.mouseY),
                                                  input.IsDown(MouseButton::Left) });

                // ImGui's GLFW backend sets the OS cursor from this every frame, so it is the only place to hide it
                if (!mGameUI.Suspended() && mGameUI.Context().HidesCursor())
                    ImGui::SetMouseCursor(ImGuiMouseCursor_None);
            });
    }

    // stops new copies; the made ones release themselves when the game UI goes
    GameUIImGuiModule::~GameUIImGuiModule()
    {
        mDrawer.Reset();
        mGameUI.Context().SetBackend(nullptr);
    }
}
