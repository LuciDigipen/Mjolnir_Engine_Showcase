/**
 * @file ImGuiModule.cpp
 * @author sumin.park
 * @brief Owns the ImGui context and runs the frame's UI drawers inside one ImGui frame.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <imguiruntime/ImGuiModule.h>

#include <core/ecs/World.h>
#include <core/fs/Paths.h>
#include <core/log/Log.h>
#include <core/log/SetupError.h>
#include <input/InputState.h>
#include <renderer/RendererModule.h>
#include <window/WindowModule.h>

#include <imgui.h>

#include <filesystem>
#include <system_error>
#include <utility>

namespace mir
{
    ImGuiModule::ImGuiModule(ModuleContext context, WindowModule &window, RendererModule &renderer)
        : mContext(context), mRenderer(renderer)
    {
        if (mRuntime.Initialize(window.Get(), renderer.Renderer()))
            return;

        // the destructor does not run for a throwing constructor
        mRuntime.Shutdown(renderer.Renderer());

        // fonts are copied by the build; any other failure is ImGui's backend, which is ours
        const std::filesystem::path fontsDir = ExecutableDir() / "fonts";
        std::error_code ec;
        if (!std::filesystem::is_directory(fontsDir, ec))
        {
            throw SetupError(ErrorCategory::BuildOutput, "The UI fonts are missing", fontsDir,
                "the fonts directory next to the engine does not exist", "Rebuild to copy the fonts.");
        }
        throw SetupError(ErrorCategory::Internal, "The UI layer could not start", {},
            "the log above names the ImGui step that failed",
            "This is an engine bug. Report it with the log attached: " + LogFilePath().string());
    }

    ImGuiModule::~ImGuiModule() { mRuntime.Shutdown(mRenderer.Renderer()); }

    Registration ImGuiModule::AddDrawer(DrawerGroup group, std::function<void()> drawer)
    {
        return mDrawers[static_cast<std::size_t>(group)].Add(std::move(drawer));
    }

    void ImGuiModule::RunUI()
    {
        mRuntime.BeginFrame();
        for (auto &group : mDrawers)
            group.Run();
        mRenderer.Renderer().SetImGuiDrawData(mRuntime.EndFrame());

        if (InputState *input = mContext.world.TryResource<InputState>())
        {
            const ImGuiIO &io = ImGui::GetIO();
            input->SetUiCapture(io.WantCaptureKeyboard, io.WantCaptureMouse);
        }
    }
}
