/**
 * @file main.cpp
 * @author Quy Dong Ta
 * @brief Device regression for render targets: a view drawn into one reaches the screen through ImGui, raster
 *        and traced, and resizing or releasing one under validation leaves no errors.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <core/fs/FileIO.h>
#include <core/log/Log.h>
#include <renderer/Shapes.h>
#include <renderer/VulkanRenderer.h>
#include <window/Window.h>

#include <imgui.h>

#include <array>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
    void Require(bool condition, const char *message)
    {
        if (!condition)
            throw std::runtime_error(message);
    }

    constexpr uint32_t kWidth = 640;
    constexpr uint32_t kHeight = 360;
}

int main(int argc, char **argv)
{
    const std::string prefix = argc > 1 ? argv[1] : "render-target-gpu";
    mir::LogConfig logging;
    logging.filePath = prefix + ".log";
    mir::InitLog(logging);
    try
    {
        auto window = mir::Window::Create(
            {.mWidth = kWidth, .mHeight = kHeight, .mTitle = "Render target GPU check", .mStartHidden = true});
        mir::VulkanRenderer renderer;
        struct Shutdown
        {
            mir::VulkanRenderer &mRenderer;
            bool mActive = true;
            ~Shutdown()
            {
                if (mActive)
                    mRenderer.Shutdown();
            }
        } shutdown{renderer};
        Require(renderer.Initialize({window.get(), "RenderTargetGpuCheck", true}), "Renderer initialization failed");

        ImGui::CreateContext();
        struct ImGuiShutdown
        {
            mir::VulkanRenderer &mRenderer;
            bool mBackend = false;
            ~ImGuiShutdown()
            {
                if (mBackend)
                    mRenderer.ShutdownImGuiVulkanBackend();
                ImGui::DestroyContext();
            }
        } imguiShutdown{renderer};
        ImGuiIO &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(float(kWidth), float(kHeight));
        io.DeltaTime = 1.0f / 60.0f;
        Require(renderer.InitImGuiVulkanBackend(), "ImGui Vulkan backend initialization failed");
        imguiShutdown.mBackend = true;

        renderer.SetAmbientColor(glm::vec3(1));
        renderer.SetDirectionalLight(glm::vec3(0, -1, 0), glm::vec3(0));
        renderer.SetPostProcessing({});
        auto quad = mir::MakeQuad();
        for (auto &vertex : quad.vertices)
            vertex.pos.z = 0.5f;
        const auto mesh = renderer.CreateMesh(quad.vertices, quad.indices);
        Require(!mesh.IsNull(), "Quad mesh creation failed");
        mir::DrawItem item;
        item.mesh = mesh;
        item.material = renderer.ErrorMaterial();

        const uint64_t errorsBefore = mir::ErrorCount();
        Require(renderer.CreateRenderTarget(0, 4).IsNull(), "A target with no width was made");
        Require(renderer.CreateRenderTarget(4, mir::kMaxRenderTargetSize + 1).IsNull(), "An oversized target was made");
        Require(mir::ErrorCount() == errorsBefore + 2, "A refused target was not reported");
        const uint64_t expectedErrors = mir::ErrorCount();

        const auto target = renderer.CreateRenderTarget(128, 64);
        Require(!target.IsNull(), "Render target creation failed");
        Require(renderer.RenderTargetSize(target).width == 128 && renderer.RenderTargetSize(target).height == 64,
            "Render target has the wrong size");

        // the screen shows nothing of its own, only the target's image across it
        std::array<mir::RenderView, 2> views{};
        views[1].target = target;
        views[1].items = std::span(&item, 1);

        uint64_t shown = 0;
        const auto frame = [&](bool trace) {
            window->PollEvents();
            Require(!window->ShouldClose(), "GPU check window closed early");
            ImGui::NewFrame();
            if (shown != 0)
                ImGui::GetBackgroundDrawList()->AddImage(ImTextureRef(static_cast<ImTextureID>(shown)), ImVec2(0, 0),
                    io.DisplaySize);
            ImGui::Render();
            renderer.SetImGuiDrawData(ImGui::GetDrawData());
            if (trace)
                renderer.TraceFrame(views);
            else
                renderer.DrawFrame(views);
        };
        const auto capture = [&](const std::string &name, bool trace) {
            for (int i = 0; i < 3; ++i)
                frame(trace);
            const std::string path = prefix + "-" + name + ".png";
            renderer.CaptureNextFrame(path);
            frame(trace);
            const auto bytes = mir::ReadBytes(path);
            Require(bytes.has_value(), "Frame capture missing");
            return *bytes;
        };

        // shown before any view draws into it: cleared, never an undefined image
        shown = renderer.ImGuiTexture(target);
        Require(shown != 0, "Render target has no ImGui texture");
        const auto quadItems = views[1].items;
        views[1].items = {};
        views[1].target = mir::kNullRenderTarget;
        const auto fresh = capture("fresh", false);
        views[1].target = target;
        views[1].items = quadItems;

        const auto drawn = capture("drawn", false);
        views[1].items = {};
        const auto empty = capture("empty", false);
        Require(drawn != empty, "The quad drawn into the target did not reach the screen");
        Require(fresh != drawn, "A fresh target already showed the quad");

        views[1].items = quadItems;
        Require(renderer.ResizeRenderTarget(target, 200, 100), "Render target resize failed");
        Require(renderer.RenderTargetSize(target).width == 200, "Resize kept the old size");
        const uint64_t resizedId = renderer.ImGuiTexture(target);
        Require(resizedId != 0 && resizedId != shown, "A resized target kept its old ImGui texture");
        // the UI built before the resize still shows the old image for one frame
        frame(false);
        shown = resizedId;
        const auto resized = capture("resized", false);
        Require(resized != empty, "The resized target did not reach the screen");

        if (renderer.RayTracingSupported())
        {
            const auto traced = capture("traced", true);
            views[1].items = {};
            const auto tracedEmpty = capture("traced-empty", true);
            Require(traced != tracedEmpty, "A traced frame did not draw its target view");
            views[1].items = quadItems;
        }

        Require(renderer.ReleaseRenderTarget(target), "Render target release failed");
        Require(renderer.ImGuiTexture(target) == 0, "A released target kept an ImGui texture");
        Require(!renderer.ReleaseRenderTarget(target), "A target was released twice");
        // the UI built before the release still shows it for one frame
        views[1].target = mir::kNullRenderTarget;
        views[1].items = {};
        frame(false);
        shown = 0;
        for (int i = 0; i < 4; ++i)
            frame(false);

        std::vector<mir::RenderTargetHandle> many;
        for (uint32_t i = 0; i < mir::kMaxRenderTargets; ++i)
        {
            many.push_back(renderer.CreateRenderTarget(16, 16));
            Require(!many.back().IsNull(), "Could not make the most targets allowed");
        }
        frame(false);
        for (const auto handle : many)
            Require(renderer.ReleaseRenderTarget(handle), "Releasing one of many targets failed");
        for (int i = 0; i < 3; ++i)
            frame(false);

        Require(mir::ErrorCount() == expectedErrors, "GPU validation or renderer errors were logged");
        const bool traced = renderer.RayTracingSupported();
        imguiShutdown.mBackend = false;
        renderer.ShutdownImGuiVulkanBackend();
        renderer.Shutdown();
        shutdown.mActive = false;
        Require(mir::ErrorCount() == expectedErrors, "Shutting down logged errors");
        MIR_LOG_INFO("Render target GPU check passed (traced frames checked: {})", traced);
        return 0;
    }
    catch (const std::exception &error)
    {
        MIR_LOG_ERROR("Render target GPU check: {}", error.what());
        return 1;
    }
}
