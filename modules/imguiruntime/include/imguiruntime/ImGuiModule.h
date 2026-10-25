/**
 * @file ImGuiModule.h
 * @author sumin.park
 * @brief Owns the ImGui context and runs the frame's UI drawers inside one ImGui frame.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <imguiruntime/ImGuiRuntime.h>

#include <core/ecs/ModuleContext.h>
#include <core/ecs/Registration.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace mir
{
    class RendererModule;
    class WindowModule;

    class ImGuiModule
    {
    public:
        // throws SetupError when ImGui cannot start: BuildOutput without the fonts, else Internal
        ImGuiModule(ModuleContext context, WindowModule &window, RendererModule &renderer);
        ~ImGuiModule();

        ImGuiModule(const ImGuiModule &) = delete;
        ImGuiModule &operator=(const ImGuiModule &) = delete;

        // drawn in registration order, between the frame's BeginFrame and EndFrame
        [[nodiscard]] Registration AddDrawer(std::function<void()> drawer);

        // BeginFrame, the drawers, EndFrame; hands the draw data to the renderer and the
        // frame's keyboard and mouse capture to InputState, for this frame's systems
        void RunUI();

        const ImGuiRuntime &Runtime() const { return mRuntime; }

    private:
        struct Drawer
        {
            uint64_t id;
            std::function<void()> draw;
        };

        ModuleContext mContext;
        RendererModule &mRenderer;
        ImGuiRuntime mRuntime;
        std::vector<Drawer> mDrawers;
        uint64_t mNextDrawerId = 0;

        // drawer tokens hold it weakly, to assert this module outlives them
        std::shared_ptr<const int> mAlive = std::make_shared<const int>(0);
    };
}
