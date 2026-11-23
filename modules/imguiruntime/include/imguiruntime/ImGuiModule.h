/**
 * @file ImGuiModule.h
 * @author sumin.park
 * @brief Owns the ImGui context and runs the frame's UI drawers inside one ImGui frame, opened before the
 *        frame's systems and closed just before Render.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <imguiruntime/ImGuiRuntime.h>

#include <core/ecs/ModuleContext.h>
#include <core/RegistrationList.h>

#include <cstddef>
#include <cstdint>
#include <functional>

namespace mir
{
    class RendererModule;
    class WindowModule;

    // Every Tools drawer runs before the frame's systems, then every Game drawer, then every Late
    // drawer, after LateUpdate. An order that follows no dependency edge needs a boundary: the
    // game UI fits the scene viewport a tool drawer, the editor's, writes the same frame, and the
    // editor's scene pick waits for the game UI's hit test of the same frame
    enum class DrawerGroup : uint8_t
    {
        Tools,
        Game,
        Late,
        kCount
    };

    class ImGuiModule
    {
    public:
        // throws SetupError when ImGui cannot start: BuildOutput without the fonts, else Internal
        ImGuiModule(ModuleContext context, WindowModule &window, RendererModule &renderer);
        ~ImGuiModule();

        ImGuiModule(const ImGuiModule &) = delete;
        ImGuiModule &operator=(const ImGuiModule &) = delete;

        // drawn by group, then in registration order, between the frame's BeginFrame and EndFrame
        [[nodiscard]] Registration AddDrawer(DrawerGroup group, std::function<void()> drawer);

        // BeginFrame and the Tools drawers, then the frame's keyboard and mouse capture to
        // InputState, for this frame's systems
        void BeginUI();
        // the Game and Late drawers and EndFrame, then the draw data to the renderer. It runs in
        // the BeforeRender slot of the scheduler that updates, so the game UI reads this frame's world
        void EndUI();
        // both, for a frame no systems update, such as a scene's loading wait
        void RunUI();

        // EndUI in the scheduler's BeforeRender slot; this module's own scheduler has it already
        [[nodiscard]] Registration EndUIOn(SystemScheduler &scheduler);
        void SetInputWorld(World *world) { mInputWorld = world != nullptr ? world : &mContext.world; }

        const ImGuiRuntime &Runtime() const { return mRuntime; }

    private:
        ModuleContext mContext;
        World *mInputWorld;
        RendererModule &mRenderer;
        ImGuiRuntime mRuntime;
        RegistrationList<std::function<void()>> mDrawers[static_cast<std::size_t>(DrawerGroup::kCount)];
        bool mInFrame = false;
        Registration mEndUI;
    };
}
