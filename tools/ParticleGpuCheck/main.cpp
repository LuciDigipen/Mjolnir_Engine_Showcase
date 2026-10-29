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
#include "ParticleRenderCheck.h"

#include <assets/AssetCache.h>
#include <assets/AssetManifest.h>
#include <core/fs/Paths.h>
#include <core/log/Log.h>
#include <core/platform/CrashHandler.h>
#include <renderer/CameraMath.h>
#include <particles_gpu/ParticlesGpuModule.h>
#include <particles/ParticleRuntime.h>
#include <particles_runtime/ParticlesRuntimeModule.h>
#include <renderer/VulkanRenderer.h>
#include <window/Window.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#if MIR_PARTICLE_RENDERDOC
#include <renderdoc_app.h>
#include <Windows.h>
#endif

#include <array>
#include <cmath>
#include <exception>
#include <filesystem>
#include <memory>
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
        auto particles = std::make_unique<mir::ParticlesGpuModule>(renderer);
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
        auto manifest = mir::AssetManifest::LoadFile(mir::ExecutableDir() / "particle_gpu_cooked" / mir::kAssetManifestFileName);
        Require(manifest.has_value(), "Missing cooked manifest");
        mir::AssetCache cache(&*manifest, mir::ExecutableDir() / "particle_gpu_cooked");
        auto cookedEmitter = mir::LoadParticleEmitterAsset(cache,
            mir::MakeAssetId("assets/exhaust.emitter.json"));
        Require(cookedEmitter.mEmitter.has_value(), "Missing cooked exhaust script");
        mir::ParticleSubmission input;
        input.scriptKey = cookedEmitter.mEmitter->script.value;
        input.scriptCode = cookedEmitter.mEmitter->scriptCode;
        auto accelerated = mir::LoadParticleEmitterAsset(cache, mir::MakeAssetId("assets/accelerated.emitter.json"));
        Require(accelerated.mEmitter.has_value(), "Missing alternate cooked script");
        Require(accelerated.mEmitter->script != cookedEmitter.mEmitter->script, "Changed stack reused old script identity");
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
            particles->Submit(std::span(&p, 1));
            draw();
        };
        auto check = [&](uint32_t low, uint32_t high, const char *message)
        {
            auto stats = particles->Stats();
            MIR_LOG_INFO("{}: completed={}, alive={}, spawned={}, dropped={}", message,
                stats.completedFrame, stats.alive, stats.spawned, stats.dropped);
            Require(stats.completedFrame != 0 && stats.alive >= low && stats.alive <= high, message);
        };
        auto quiet = input;
        quiet.spawns = 5000;
        particles->Submit(std::span(&quiet, 1));
        quiet.spawns = 0;
        particles->Submit(std::span(&quiet, 1));
        Require(particles->Stats().droppedSubmitSpawns == 5000,
            "Superseded snapshots and upload-budget drops were not counted");
        for (int i = 0; i < 5; ++i) submit(quiet);
        check(0, 0, "Zero births leave the empty pool empty");
        quiet.capacity = 0;
        const auto invalid = particles->Stats().rejectedSubmissions;
        submit(quiet);
        Require(particles->Stats().rejectedSubmissions == invalid + 1,
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
        // Replace scripts repeatedly while previous frame slots remain in flight.
        for (int i = 0; i < 12; ++i)
        {
            const auto &definition = i % 2 ? *cookedEmitter.mEmitter : *accelerated.mEmitter;
            input.scriptKey = definition.script.value;
            input.scriptCode = definition.scriptCode;
            submit(input);
        }
        check(29, 31, "Changed cooked stacks replace pipelines without changing lifetime counts");
        input.scriptKey = cookedEmitter.mEmitter->script.value;
        input.scriptCode = cookedEmitter.mEmitter->scriptCode;
        // Pause advances neither age nor emission.
        input.seconds = 0;
        input.spawns = 0;
        const auto pausedAlive = particles->Stats().alive;
        for (int i = 0; i < 8; ++i) submit(input);
        Require(particles->Stats().alive == pausedAlive, "Pause changed live count");
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
        Require(particles->Stats().dropped == mir::kGpuParticleSpawnsPerFrame, "Full pool did not count dropped births");
        // Actual window resize exercises per-frame HDR images and descriptor replacement.
        glfwSetWindowSize(static_cast<GLFWwindow *>(window->NativeHandleForImGui()), 800, 450);
        for (int i = 0; i < 8; ++i) submit(input);
        check(mir::kGpuParticleCapacity, mir::kGpuParticleCapacity, "Resize preserves the persistent pool");
        particles->Submit({});
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
        particles->Submit(many);
        draw();
        for (auto &p : many) p.spawns = 0;
        for (int i = 0; i < 5; ++i) { particles->Submit(many); draw(); }
        check(64, 64, "All instance slots have disjoint ranges");
        ++many[0].generation;
        for (int i = 0; i < 5; ++i) { particles->Submit(many); draw(); }
        check(63, 63, "Generation replacement preserves other instances");
        many[0].capacity = mir::kGpuParticleCapacity;
        const auto rejected = particles->Stats().rejectedSubmissions;
        for (int i = 0; i < 5; ++i) { particles->Submit(many); draw(); }
        check(63, 63, "Range exhaustion preserves existing instances");
        Require(particles->Stats().rejectedSubmissions > rejected, "Range exhaustion was not counted");
        particles->Submit({});
        for (int i = 0; i < 5; ++i) draw();
        check(0, 0, "All instance ranges retired");
        // A system owns two emitter instances, the scene proxy publishes distinct GPU identities.
        mir::ParticleEffect system;
        system.emitters = {mir::AssetId{101}, mir::AssetId{102}};
        mir::ParticleEmitterAsset firstEmitter = *cookedEmitter.mEmitter;
        firstEmitter.texture = mir::AssetId{201};
        firstEmitter.capacity = 64;
        firstEmitter.lifetime = 1;
        auto secondEmitter = firstEmitter;
        secondEmitter.spawnRate = 120;
        system.emitterDefinitions = {firstEmitter, secondEmitter};
        mir::ParticleRuntime runtime;
        const auto systemHandle = runtime.Start(mir::AssetId{301}, system);
        Require(!systemHandle.IsNull(), "Two-emitter system did not start");
        for (int i = 0; i < 10; ++i)
        {
            runtime.Advance(1.0f / 60);
            const auto snapshot = mir::BuildParticleRenderData(runtime, true);
            Require(snapshot.size() == 2 && snapshot[0].index != snapshot[1].index,
                "System emitters share a GPU instance identity");
            particles->Submit(snapshot);
            draw();
        }
        Require(runtime.Pause(systemHandle, true), "Two-emitter system pause failed");
        for (int i = 0; i < 5; ++i)
        {
            runtime.Advance(1.0f / 60);
            particles->Submit(mir::BuildParticleRenderData(runtime, true));
            draw();
        }
        check(30, 30, "One GPU range per emitter in a two-emitter system");
        renderer.ReleaseSceneResources();
        for (int i = 0; i < 5; ++i) draw();
        check(0, 0, "Renderer scene hook clears the external FX provider");
        input.capacity = 128;
        input.lifetime = 1;
        input.spawns = 32;
        ++input.generation;
        submit(input);
        input.spawns = 0;
        for (int i = 0; i < 6; ++i) submit(input);
        check(32, 32, "Range recycled after unload");
        // Unregistration waits only at teardown, retires the old backend before its storage dies.
        particles.reset();
        draw();
        particles = std::make_unique<mir::ParticlesGpuModule>(renderer);
        input.spawns = 32;
        submit(input);
        input.spawns = 0;
        for (int i = 0; i < 6; ++i) submit(input);
        check(32, 32, "FX provider unregisters and can be replaced");
        // Asynchronous samples prove swept births and tile shifts through the production path.
        auto position = [](double x, double y = 0.0, double z = 0.0)
        {
            return *mir::WorldPosition::FromDoubleMeters(glm::dvec3(x, y, z));
        };
        auto sample = input;
        sample.velocity = glm::vec3(0);
        sample.lifetime = 120;
        sample.spawns = 4;
        sample.swept = true;
        sample.explicitTile = true;
        sample.anchor = position(4);
        sample.emissionStart = position(0);
        sample.tileOrigin = position(0);
        sample.births.clear();
        for (uint32_t b = 0; b < 4; ++b)
            sample.births.push_back({float(b + 1) / 4, (1 - float(b + 1) / 4) * sample.seconds, 10 + b, 0});
        ++sample.generation;
        submit(sample);
        sample.spawns = 0;
        sample.births.clear();
        sample.seconds = 0;
        for (int f = 0; f < 5; ++f) submit(sample);
        auto sweptStats = particles->Stats();
        Require(sweptStats.alive == 4 && sweptStats.firstSequence == 10 &&
            std::abs(sweptStats.firstPosition.Meters().x - 1) < 0.001,
            "Swept birth positions or deterministic sequence differ");
        const auto shifts = sweptStats.tileShifts;
        for (int f = 1; f <= 8; ++f)
        {
            sample.anchor = sample.tileOrigin = position(f * 64.0);
            submit(sample);
        }
        auto tileStats = particles->Stats();
        Require(tileStats.alive == 4 && tileStats.tileShifts > shifts &&
            std::abs(tileStats.firstPosition.Meters().x - 1) < 0.001,
            "Tile changes moved or dropped persistent particles");
        if constexpr (mir::kLargeWorld)
        {
            ++sample.generation;
            sample.anchor = position(1000004);
            sample.emissionStart = sample.tileOrigin = position(1000000);
            sample.spawns = 4;
            for (uint32_t b = 0; b < 4; ++b) sample.births.push_back({float(b + 1) / 4, 0, 10 + b, 0});
            submit(sample);
            sample.spawns = 0;
            sample.births.clear();
            for (int f = 0; f < 5; ++f) submit(sample);
            const auto farStats = particles->Stats();
            Require(farStats.alive == 4 && std::abs(farStats.firstPosition.Meters().x - 1000001) < 0.01,
                "Translated GPU births exceed one centimetre error");
        }
        else
        {
            const auto rejectedFar = particles->Stats().rejectedSubmissions;
            sample.anchor = position(1000000);
            submit(sample);
            Require(particles->Stats().rejectedSubmissions == rejectedFar + 1,
                "Float-world GPU submissions must reject remote unsupported origins");
        }
        // Exercise cooked variation, wind, lifetime curves, flipbooks and texture replacement.
        const auto advanced = mir::LoadParticleEmitterAsset(cache, mir::MakeAssetId("assets/advanced.emitter.json"));
        Require(advanced.mEmitter.has_value(), "Missing advanced cooked GPU script");
        sample.scriptKey = advanced.mEmitter->script.value;
        sample.scriptCode = advanced.mEmitter->scriptCode;
        sample.textureKey = advanced.mEmitter->texture.value;
        sample.textureWidth = advanced.mEmitter->textureWidth;
        sample.textureHeight = advanced.mEmitter->textureHeight;
        sample.texturePixels = advanced.mEmitter->texturePixels;
        sample.anchor = sample.emissionStart = sample.tileOrigin = position(0);
        sample.swept = sample.explicitTile = false;
        sample.seed = 1702;
        sample.wind = {2, 0, -1};
        sample.velocity = {0, 0, 4};
        sample.seconds = 1.0f / 60;
        sample.lifetime = 3;
        sample.size = 0.15f;
        sample.color.a = 0.75f;
        glm::vec3 seededVelocity{};
        for (int repeat = 0; repeat < 2; ++repeat)
        {
            ++sample.generation;
            sample.spawns = 1;
            submit(sample);
            sample.spawns = 0;
            for (int f = 0; f < 12; ++f) submit(sample);
            const auto advancedStats = particles->Stats();
            Require(advancedStats.alive == 1 && advancedStats.firstSize > sample.size &&
                advancedStats.firstAlpha < sample.color.a && advancedStats.firstFrame >= 1,
                "Cooked lifetime curves or flipbook did not advance");
            if (repeat == 0) seededVelocity = advancedStats.firstVelocity;
            else Require(glm::length(advancedStats.firstVelocity - seededVelocity) < 0.0001f,
                "Repeated seed and timeline changed GPU velocity");
        }
        for (int reload = 0; reload < 6; ++reload)
        {
            sample.texturePixels[0] ^= 0xff;
            submit(sample);
        }
        MIR_LOG_INFO("Moving exhaust: swept births, tile continuity, translated precision, seeded curves and flipbook checks passed");
        // Ribbons use the production CPU runtime, immutable bridge and FX renderer.
        mir::ParticleRuntime ribbons;
        mir::ParticleEmitterAsset trail;
        trail.simulationTarget = mir::ParticleSimulationTarget::CPU;
        trail.texture = cookedEmitter.mEmitter->texture;
        trail.textureWidth = cookedEmitter.mEmitter->textureWidth;
        trail.textureHeight = cookedEmitter.mEmitter->textureHeight;
        trail.texturePixels = cookedEmitter.mEmitter->texturePixels;
        trail.spawnRate = 0;
        trail.spawnPerMeter = 12;
        trail.capacity = 128;
        trail.lifetime = 4;
        trail.size = 0.12f;
        trail.color = {0.8f,0.9f,1,0.6f};
        trail.spawnModules = {{"RibbonSpawn", {0.1f}}};
        trail.updateModules = {{"RibbonAge", {}}, {"RibbonWind", {}}, {"RibbonWidth", {2}}, {"RibbonFade", {}}};
        mir::ParticleEffect trailEffect;
        trailEffect.emitters = {{1}};
        trailEffect.emitterDefinitions = {trail};
        trailEffect.parameters.wind = {0,0.3f,0};
        const auto trailHandle = ribbons.Start({2},trailEffect);
        Require(!trailHandle.IsNull(), "CPU ribbon fixture failed to start");
        auto drawRibbons = [&]
        {
            particles->Submit({},mir::BuildRibbonRenderData(ribbons));
            draw();
        };
        for (int f = 1; f <= 90; ++f)
        {
            mir::WorldPose pose;
            pose.position = *mir::WorldPosition::FromMeters(mir::WorldVec3(-2.0 + f / 20.0, 0.3 * std::sin(f / 20.0), -6));
            Require(ribbons.SetPose(trailHandle,pose), "CPU ribbon pose rejected");
            ribbons.Advance(1.0f/60);
            drawRibbons();
        }
        split = !rt;
        for (int f = 0; f < 4; ++f) drawRibbons();
        split = false;
        if constexpr (sizeof(mir::WorldReal) != sizeof(float))
        {
            const auto oldCamera = view.cameraPosition;
            const auto snapshot = mir::BuildRibbonRenderData(ribbons);
            auto translated = snapshot;
            for (auto &ribbon : translated)
                for (auto &point : ribbon.points)
                    point.position = *mir::WorldPosition::FromMeters(point.position.Meters() + mir::WorldVec3(1000000,0,0));
            view.cameraPosition = *mir::WorldPosition::FromMeters(oldCamera.Meters() + mir::WorldVec3(1000000,0,0));
            for (int f = 0; f < 4; ++f) { particles->Submit({},translated); draw(); }
            view.cameraPosition = oldCamera;
        }
        renderer.CaptureNextFrame(output);
        drawRibbons();
        particles->Clear();
        for (int f = 0; f < 3; ++f) draw();
        MIR_LOG_INFO("CPU ribbon uploads, split views, remote camera and clear passed");
        RunParticleRenderCheck(renderer,*particles,*window,*cookedEmitter.mEmitter,output,rt,mode=="dlss");
        {
            const auto measured = particles->Stats();
            Require(measured.gpuBytes > 0 && measured.gpuHighWaterBytes >= measured.gpuBytes,
                "GPU owned-byte diagnostics did not account for live buffers");
            Require(std::isfinite(measured.simulationGpuMs) && std::isfinite(measured.prepareGpuMs) && std::isfinite(measured.drawGpuMs),
                "Particle timestamp diagnostics returned nonfinite results");
            if (measured.gpuTimingAvailable)
                Require(measured.simulationGpuMs + measured.prepareGpuMs + measured.drawGpuMs > 0,
                    "Supported particle timestamp queries never completed");
            particles.reset();
            mir::ParticleGpuBudgets caps;
            caps.snapshotBytes = 1;
            particles = std::make_unique<mir::ParticlesGpuModule>(renderer,caps);
            particles->Submit(std::span(&input,1)); draw();
            Require(particles->Stats().rejectedSubmissions > 0,"Snapshot byte cap was ignored");
            particles.reset();
            caps = {}; caps.birthUploads = 1; caps.textureBytes = 8; caps.ribbonVertices = 6;
            particles = std::make_unique<mir::ParticlesGpuModule>(renderer,caps);
            auto capped = input;
            capped.capacity = 32; capped.spawns = 3; capped.seconds = 0; capped.lifetime = 30;
            capped.textureKey = 777; capped.textureWidth = capped.textureHeight = 2;
            capped.texturePixels.assign(16,255);
            mir::RibbonSubmission strip;
            for (uint64_t i=0;i<3;++i)
                strip.points.push_back({*mir::WorldPosition::FromMeters(mir::WorldVec3(double(i)-1,0,-6)),double(i),1,i,0.2f,glm::vec4(1)});
            particles->Submit(std::span(&capped,1),std::span(&strip,1));
            for (int f=0;f<5;++f) draw();
            const auto bounded = particles->Stats();
            Require(bounded.alive == 1 && bounded.droppedSubmitSpawns == 2,"Birth upload budget failed");
            Require(bounded.deferredTextures > 0,"Texture memory cap did not defer new images");
            Require(bounded.droppedRibbonSegments > 0,"Ribbon vertex upload cap was ignored");
            particles->Clear(); for(int f=0;f<4;++f) draw();
            Require(particles->Stats().alive == 0,"Capped renderer retained cleared instances");
            particles.reset();
            caps = {}; caps.textureUploadBytes = 4;
            particles = std::make_unique<mir::ParticlesGpuModule>(renderer,caps);
            capped.spawns = 0;
            particles->Submit(std::span(&capped,1));
            for(int f=0;f<4;++f) draw();
            Require(particles->Stats().deferredTextures > 0,"Texture upload cap did not defer an otherwise resident image");
            particles->Clear(); for(int f=0;f<3;++f) draw();
            MIR_LOG_INFO("Particle budgets and asynchronous CPU/GPU/memory diagnostics passed");
        }
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
