/**
 * @file ImGuiModule.cpp
 * @author sumin.park
 * @brief Owns the ImGui context and runs the frame's UI drawers inside one ImGui frame, opened before the
 *        frame's systems and closed just before Render.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <imguiruntime/ImGuiModule.h>

#include <core/ecs/SystemScheduler.h>
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
        : mContext(context), mInputWorld(&context.world), mRenderer(renderer)
    {
        if (mRuntime.Initialize(window.Get(), renderer.Renderer()))
        {
            mEndUI = EndUIOn(context.scheduler);
            return;
        }

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

    void ImGuiModule::BeginUI()
    {
        // a frame whose systems never reached Render still closes, since ImGui asserts on a second NewFrame
        if (mInFrame)
            EndUI();

        mRuntime.BeginFrame();
        mInFrame = true;
        mDrawers[static_cast<std::size_t>(DrawerGroup::Tools)].Run();

        // NewFrame sets ImGui's capture, so it is final here
        if (InputState *input = mInputWorld->TryResource<InputState>())
        {
            const ImGuiIO &io = ImGui::GetIO();
            input->SetUiCapture(io.WantCaptureKeyboard, io.WantCaptureMouse);
        }
    }

    void ImGuiModule::EndUI()
    {
        if (!mInFrame)
            return;

        mDrawers[static_cast<std::size_t>(DrawerGroup::Game)].Run();
        mDrawers[static_cast<std::size_t>(DrawerGroup::Late)].Run();
        mInFrame = false;
        mRenderer.Renderer().SetImGuiDrawData(mRuntime.EndFrame());
    }

    void ImGuiModule::RunUI()
    {
        BeginUI();
        EndUI();
    }

    Registration ImGuiModule::EndUIOn(SystemScheduler &scheduler)
    {
        return scheduler.AddFrameSlot(FrameSlot::BeforeRender, [this](SystemContext &) { EndUI(); });
    }
}
