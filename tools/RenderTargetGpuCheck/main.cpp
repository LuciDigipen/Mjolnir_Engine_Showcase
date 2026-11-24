/**
 * @file main.cpp
 * @author Quy Dong Ta
 * @brief Device regression for render targets: a view drawn into one reaches the screen through ImGui, raster
 *        and traced, resizing or releasing one under validation leaves no errors, and RenderSystem draws a
 *        RenderTexture camera's picture into the image it publishes.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <core/ecs/CommandBuffer.h>
#include <core/ecs/TransformHierarchy.h>
#include <core/ecs/World.h>
#include <core/fs/FileIO.h>
#include <core/log/Log.h>
#include <renderer/RenderSystem.h>
#include <renderer/Shapes.h>
#include <renderer/VulkanRenderer.h>
#include <renderer/components/RenderTexture.h>
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

        // shown, then resized or released before any frame drew it: the UI built still samples the old image
        for (const bool release : { false, true })
        {
            const auto early = renderer.CreateRenderTarget(32, 32);
            Require(!early.IsNull(), "Render target creation failed");
            window->PollEvents();
            ImGui::NewFrame();
            ImGui::GetBackgroundDrawList()->AddImage(
                ImTextureRef(static_cast<ImTextureID>(renderer.ImGuiTexture(early))), ImVec2(0, 0), io.DisplaySize);
            ImGui::Render();
            renderer.SetImGuiDrawData(ImGui::GetDrawData());
            if (release)
                Require(renderer.ReleaseRenderTarget(early), "Releasing a target never drawn failed");
            else
                Require(renderer.ResizeRenderTarget(early, 48, 48), "Resizing a target never drawn failed");
            renderer.DrawFrame(views);
            if (!release)
                Require(renderer.ReleaseRenderTarget(early), "Render target release failed");
            for (int i = 0; i < 3; ++i)
                frame(false);
        }

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

        // RenderSystem: a RenderTexture camera's image, published as the binding does, drawn by the system
        {
            mir::World world;
            world.EmplaceResource<mir::RenderSettings>();
            world.EmplaceResource<mir::PostProcessingSettings>();
            mir::CommandBuffer commands;
            mir::SystemContext context{world, commands, 1.0f / 60.0f, 0.0, 0};
            const auto place = [&](mir::Entity entity, mir::WorldVec3 position) {
                mir::AddTransform(world, entity, mir::Transform(position));
                mir::ResolveWorld(world, entity);
            };
            const auto cube = mir::MakeCube();
            const auto cubeMesh = renderer.CreateMesh(cube.vertices, cube.indices);
            Require(!cubeMesh.IsNull(), "Cube mesh creation failed");
            const mir::Entity box = world.CreateEntity();
            place(box, mir::WorldVec3(0, 0, -3));
            world.AddComponent<mir::MeshRenderer>(box, mir::MeshRenderer{.mesh = cubeMesh, .material = renderer.ErrorMaterial()});
            // the screen's camera looks away from the cube, the minimap's at it
            const mir::Entity screenCamera = world.CreateEntity();
            place(screenCamera, mir::WorldVec3(0, 0, -6));
            world.AddComponent<mir::Camera>(screenCamera, mir::Camera{.mActive = true});
            const mir::Entity minimap = world.CreateEntity();
            place(minimap, mir::WorldVec3(0, 0, 0));
            world.AddComponent<mir::Camera>(minimap, mir::Camera{});
            world.AddComponent<mir::RenderTexture>(minimap, mir::RenderTexture{96, 48});

            mir::RenderSystem system(renderer);
            system.OnStart(context);
            mir::CameraImages images;
            const auto systemFrame = [&] {
                window->PollEvents();
                system.PublishCameraImages(world, images);
                ImGui::NewFrame();
                if (!images.mImages.empty())
                    ImGui::GetBackgroundDrawList()->AddImage(
                        ImTextureRef(static_cast<ImTextureID>(images.mImages[0].mUITexture)), ImVec2(0, 0),
                        io.DisplaySize);
                ImGui::Render();
                renderer.SetImGuiDrawData(ImGui::GetDrawData());
                system.OnUpdate(context);
            };
            const auto systemCapture = [&](const std::string &name) {
                for (int i = 0; i < 3; ++i)
                    systemFrame();
                const std::string path = prefix + "-" + name + ".png";
                renderer.CaptureNextFrame(path);
                systemFrame();
                const auto bytes = mir::ReadBytes(path);
                Require(bytes.has_value(), "Frame capture missing");
                return *bytes;
            };

            const auto withImage = systemCapture("system-minimap");
            Require(images.mImages.size() == 1 && images.mImages[0].mCamera == minimap, "The minimap was not published");
            Require(images.mImages[0].mUITexture != 0 && images.mImages[0].mWidth == 96 &&
                        images.mImages[0].mHeight == 48 && images.mImages[0].mView.mValid,
                "The minimap's image is not as its RenderTexture says");
            Require(mir::ResolvePlayCamera(world).mEntity == screenCamera, "The minimap became the Play camera");

            world.RemoveComponent<mir::RenderTexture>(minimap);
            const auto withoutImage = systemCapture("system-none");
            Require(images.mImages.empty(), "A camera without a RenderTexture was published");
            Require(withImage != withoutImage, "The minimap's picture did not reach the screen");

            world.AddComponent<mir::RenderTexture>(minimap, mir::RenderTexture{64, 64});
            systemFrame();
            Require(images.mImages.size() == 1 && images.mImages[0].mWidth == 64, "A new RenderTexture got no image");
            // as the binding does when its World goes
            system.ReleaseRenderTargets();
            for (int i = 0; i < 3; ++i)
                frame(false);
        }

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
