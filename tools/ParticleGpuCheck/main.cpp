/**
 * @file main.cpp
 * @author sumin.park
 * @brief Device regression checks through the production submission and diagnostics API.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <core/log/Log.h>
#include <core/platform/CrashHandler.h>
#include <renderer/CameraMath.h>
#include <renderer/VulkanRenderer.h>
#include <window/Window.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#if MIR_PARTICLE_RENDERDOC
#include <renderdoc_app.h>
#include <Windows.h>
#endif

#include <array>
#include <exception>
#include <filesystem>
#include <stdexcept>
#include <string_view>

namespace
{
    void Require(bool condition, const char *message)
    {
        if (!condition) throw std::runtime_error(message);
    }
}

int main(int argc, char **argv)
{
    const std::filesystem::path output = argc > 1 ? argv[1] : "particle-gpu.png";
    const std::string_view mode = argc > 2 ? argv[2] : "raster";
    mir::LogConfig logging;
    logging.filePath = output.string() + ".log";
    mir::InitLog(logging);
    mir::InstallCrashHandler({});
    try
    {
        auto window = mir::Window::Create({.mWidth = 640, .mHeight = 360, .mTitle = "Particle GPU check"});
        window->Show();
        mir::VulkanRenderer renderer;
        Require(renderer.Initialize({window.get(), "ParticleGpuCheck", true}), "Renderer initialization failed");
        const bool rt = mode == "rt" || mode == "dlss";
        if (rt && !renderer.RayTracingSupported()) { renderer.Shutdown(); return 77; }
        if (mode == "dlss")
        {
            if (!renderer.DlssAvailable()) { renderer.Shutdown(); return 77; }
            mir::PostProcessingState effects;
            effects.antiAliasing.mode = mir::AntiAliasingMode::Dlss;
            renderer.SetPostProcessing(effects);
        }
        mir::RenderView view;
        view.cameraPosition = *mir::WorldPosition::FromMeters(mir::WorldVec3(0, 0, 8));
        view.viewProj = mir::MakeViewProjection(glm::mat4(1), mir::Camera{60, 0.1f, 100}, 640.0f / 360);
        mir::ParticleSubmission input;
        input.generation = 1;
        input.capacity = 64;
        input.seconds = 1.0f / 60;
        input.lifetime = 0.5f;
        input.spawns = 1;
#if MIR_PARTICLE_RENDERDOC
        RENDERDOC_API_1_6_0 *capture = nullptr;
        if (argc > 3 && std::string_view(argv[3]) == "capture")
        {
            const auto module = GetModuleHandleW(L"renderdoc.dll");
            const auto getApi = module ? reinterpret_cast<pRENDERDOC_GetAPI>(GetProcAddress(module, "RENDERDOC_GetAPI")) : nullptr;
            Require(getApi && getApi(eRENDERDOC_API_Version_1_6_0, reinterpret_cast<void **>(&capture)),
                "Launch the fixture with renderdoccmd capture for a GPU capture");
            capture->SetCaptureFilePathTemplate((output.string() + ".gpu").c_str());
        }
#endif
        uint64_t waits = 0, queueWaits = 0;
        bool split = false;
        auto draw = [&]
        {
            window->PollEvents();
            Require(!window->ShouldClose(), "GPU check window closed early");
            if (rt) renderer.TraceFrame(std::span(&view, 1));
            else if (split)
            {
                std::array<mir::RenderView, 2> views{view, view};
                const auto extent = renderer.SwapchainExtent();
                views[0].rect = {{0, 0}, {extent.width / 2, extent.height}};
                views[1].rect = {{int32_t(extent.width / 2), 0}, {extent.width / 2, extent.height}};
                renderer.DrawFrame(views);
            }
            else renderer.DrawFrame(std::span(&view, 1));
        };
        auto submit = [&](const mir::ParticleSubmission &p)
        {
            renderer.SubmitParticles(std::span(&p, 1));
            draw();
        };
        auto check = [&](uint32_t low, uint32_t high, const char *message)
        {
            auto stats = renderer.ParticleStats();
            MIR_LOG_INFO("{}: completed={}, alive={}, spawned={}, dropped={}", message,
                stats.completedFrame, stats.alive, stats.spawned, stats.dropped);
            Require(stats.completedFrame != 0 && stats.alive >= low && stats.alive <= high, message);
        };
        auto quiet = input;
        quiet.spawns = 5000;
        renderer.SubmitParticles(std::span(&quiet, 1));
        quiet.spawns = 0;
        renderer.SubmitParticles(std::span(&quiet, 1));
        Require(renderer.ParticleStats().droppedSubmitSpawns == 5000,
            "Superseded snapshots and upload-budget drops were not counted");
        for (int i = 0; i < 5; ++i) submit(quiet);
        check(0, 0, "Zero births leave the empty pool empty");
        quiet.capacity = 0;
        const auto invalid = renderer.ParticleStats().rejectedSubmissions;
        submit(quiet);
        Require(renderer.ParticleStats().rejectedSubmissions == invalid + 1,
            "Zero capacity submission was not rejected");
        for (uint32_t frame = 0; frame < 150; ++frame)
        {
#if MIR_PARTICLE_RENDERDOC
            if (capture && frame == 80) capture->TriggerCapture();
#endif
            submit(input);
            if (frame == 60)
            {
                check(29, 31, "Continuous emitter steady state");
                waits = renderer.MeshStreaming().deviceWaits;
                queueWaits = renderer.MeshStreaming().queueWaits;
            }
        }
        check(29, 31, "Repeated recycling remains bounded");
        Require(renderer.MeshStreaming().deviceWaits == waits && renderer.MeshStreaming().queueWaits == queueWaits,
            "Steady-state simulation waited idle");
        // Pause advances neither age nor emission.
        input.seconds = 0;
        input.spawns = 0;
        const auto pausedAlive = renderer.ParticleStats().alive;
        for (int i = 0; i < 8; ++i) submit(input);
        Require(renderer.ParticleStats().alive == pausedAlive, "Pause changed live count");
        input.seconds = 1.0f / 60;
        for (int i = 0; i < 40; ++i) submit(input);
        check(0, 0, "Stopped emitter drains to zero");
        // An exact finite burst, followed by no new births.
        ++input.generation;
        input.spawns = 32;
        submit(input);
        input.spawns = 0;
        for (int i = 0; i < 5; ++i) submit(input);
        check(32, 32, "Finite burst allocated");
        split = true;
        for (int i = 0; i < 12; ++i) submit(input);
        check(32, 32, "Multiple views do not advance simulation twice");
        split = false;
        for (int i = 0; i < 40; ++i) submit(input);
        check(0, 0, "Finite burst expired");
        ++input.generation;
        input.capacity = mir::kGpuParticleCapacity;
        input.lifetime = 120;
        input.spawns = mir::kGpuParticleSpawnsPerFrame;
        for (int i = 0; i < 24; ++i) submit(input);
        check(mir::kGpuParticleCapacity, mir::kGpuParticleCapacity, "Global pool saturation");
        Require(renderer.ParticleStats().dropped == mir::kGpuParticleSpawnsPerFrame, "Full pool did not count dropped births");
        // Actual window resize exercises per-frame HDR images and descriptor replacement.
        glfwSetWindowSize(static_cast<GLFWwindow *>(window->NativeHandleForImGui()), 800, 450);
        for (int i = 0; i < 8; ++i) submit(input);
        check(mir::kGpuParticleCapacity, mir::kGpuParticleCapacity, "Resize preserves the persistent pool");
        renderer.SubmitParticles({});
        for (int i = 0; i < 6; ++i) draw();
        check(0, 0, "Unload clears indirect counts");
        // Every logical slot gets a disjoint range, one generation can retire independently.
        std::array<mir::ParticleSubmission, mir::kGpuParticleInstances> many;
        for (uint32_t i = 0; i < many.size(); ++i)
        {
            many[i] = input;
            many[i].index = i;
            many[i].generation = 100;
            many[i].capacity = 64;
            many[i].spawns = 1;
            many[i].seconds = 0;
        }
        renderer.SubmitParticles(many);
        draw();
        for (auto &p : many) p.spawns = 0;
        for (int i = 0; i < 5; ++i) { renderer.SubmitParticles(many); draw(); }
        check(64, 64, "All instance slots have disjoint ranges");
        ++many[0].generation;
        for (int i = 0; i < 5; ++i) { renderer.SubmitParticles(many); draw(); }
        check(63, 63, "Generation replacement preserves other instances");
        many[0].capacity = mir::kGpuParticleCapacity;
        const auto rejected = renderer.ParticleStats().rejectedSubmissions;
        for (int i = 0; i < 5; ++i) { renderer.SubmitParticles(many); draw(); }
        check(63, 63, "Range exhaustion preserves existing instances");
        Require(renderer.ParticleStats().rejectedSubmissions > rejected, "Range exhaustion was not counted");
        renderer.SubmitParticles({});
        for (int i = 0; i < 5; ++i) draw();
        check(0, 0, "All instance ranges retired");
        input.capacity = 128;
        input.lifetime = 1;
        input.spawns = 32;
        ++input.generation;
        submit(input);
        input.spawns = 0;
        for (int i = 0; i < 6; ++i) submit(input);
        check(32, 32, "Range recycled after unload");
        renderer.CaptureNextFrame(output);
        submit(input);
        renderer.Shutdown();
        Require(std::filesystem::exists(output), "No GPU output capture");
        MIR_LOG_INFO("Particle GPU checks passed, mode={}, validation includes synchronization checks", mode);
        return mir::ErrorCount() == 0 ? 0 : 1;
    }
    catch (const std::exception &error)
    {
        MIR_LOG_ERROR("Particle GPU check failed: {}", error.what());
        return 1;
    }
}
