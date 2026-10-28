/**
 * @file main.cpp
 * @author sumin.park
 * @brief Bounded static composition fixture, never simulates particles.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <app/App.h>
#include <assets/AssetIdRoot.h>
#include <core/ecs/TransformHierarchy.h>
#include <core/log/Log.h>
#include <core/platform/CrashHandler.h>
#include <project/GameProject.h>
#include <renderer/PostProcessingSettings.h>
#include <renderer/RenderSettings.h>
#include <renderer/Shapes.h>
#include <renderer/components/Light.h>
#include <renderer/components/MeshRenderer.h>

#include <cstring>
#include <exception>
#include <string>
#include <string_view>

namespace
{
    class ProbeCut final : public mir::ISystem
    {
    public:
        ProbeCut(mir::Entity camera, mir::VulkanRenderer &renderer, bool cut)
            : mCamera(camera), mRenderer(renderer), mCut(cut)
        {}
        void OnUpdate(mir::SystemContext &context) override
        {
            if (mCut && context.frame == 45)
            {
                auto *transform = context.world.GetComponent<mir::Transform>(mCamera);
                transform->Translate(mir::WorldVec3(1.4, 0, 0));
                mRenderer.ResetHistory();
                MIR_LOG_INFO("Particle probe: camera cut and history reset at frame 45");
            }
        }
    private:
        mir::Entity mCamera;
        mir::VulkanRenderer &mRenderer;
        bool mCut;
    };
}

int main(int argc, char **argv)
{
    mir::LogConfig logging{};
    if (argc >= 3)
        logging.filePath = std::string(argv[2]) + ".log";
    mir::InitLog(logging);
    mir::InstallCrashHandler({});
    try
    {
        if (argc < 3 || argc > 4)
        {
            MIR_LOG_ERROR("Usage: ParticleCompositionCheck raster|rt|dlss capture.png [cut|offset|effects|off]");
            return 10;
        }
        const std::string_view mode(argv[1]);
        const std::string_view scenario = argc == 4 ? argv[3] : "still";
        if ((mode != "raster" && mode != "rt" && mode != "dlss") ||
            (scenario != "still" && scenario != "cut" && scenario != "offset" &&
                scenario != "effects" && scenario != "off"))
        {
            MIR_LOG_ERROR("Invalid particle composition fixture mode/scenario");
            return 10;
        }
        mir::AppDesc desc{};
        desc.mProject = mir::ResolveGameProject("games/EmptyScene", mir::ErrorCategory::ProjectData);
        mir::ResolveAssetIdRoot(desc.mProject);
        desc.mWidth = 960;
        desc.mHeight = 540;
        desc.mMaxFrames = 90;
        desc.mCapturePath = argv[2];
        desc.mEnableEditorLayout = false;
        desc.mNonInteractive = true;
        {
            mir::App app(desc);
            app.Splash().Close();
            auto &renderer = app.Renderer();
            const bool unavailable = (mode != "raster" && !renderer.RayTracingSupported()) ||
                (mode == "dlss" && !renderer.DlssAvailable());
            auto &world = app.GetWorld();
            const mir::WorldVec3 origin(scenario == "offset" ? 1000000.0 : 0.0, 0, 0);
            const auto anchor = mir::WorldPosition::FromMeters(origin);
            renderer.SetParticleCompositionProbe(scenario == "off" ? std::nullopt : anchor);

            const auto camera = world.CreateEntity();
            mir::AddTransform(world, camera, mir::Transform(origin + mir::WorldVec3(0, 0, 8)));
            world.AddComponent<mir::Camera>(camera, mir::Camera{ 60.0f, 0.1f, 100.0f });
            app.Systems().AddSystem<ProbeCut>(mir::SystemPhase::PostUpdate,
                mir::SystemOrder{ mir::SystemOrder::kLate }, camera, renderer, scenario == "cut");

            // Front cube must hide both shapes, rear cube must show through them.
            for (int i = 0; i < 2; ++i)
            {
                const auto cube = world.CreateEntity();
                mir::AddTransform(world, cube, mir::Transform(origin + mir::WorldVec3(i == 0 ? 0.0 : -1.7,
                    0, i == 0 ? 1.5 : -1.5), glm::quat(1, 0, 0, 0), glm::vec3(1.2f, 2.4f, 1.0f)));
                mir::MeshRenderer mesh{};
                std::memcpy(mesh.meshName, mir::kCubeMeshName.data(), mir::kCubeMeshName.size());
                std::memcpy(mesh.materialShader, "default", 7);
                mesh.tint = i == 0 ? glm::vec4(0.2f, 0.5f, 1.0f, 1) : glm::vec4(1.0f);
                world.AddComponent<mir::MeshRenderer>(cube, mesh);
            }
            const auto sky = world.CreateEntity();
            mir::AddTransform(world, sky);
            world.AddComponent<mir::Sky>(sky, mir::Sky{});
            const auto sun = world.CreateEntity();
            mir::AddTransform(world, sun);
            mir::Light light{};
            light.mType = mir::LightType::Sun;
            world.AddComponent<mir::Light>(sun, light);
            world.Resource<mir::RenderSettings>().rayTracing = mode != "raster";
            auto &effects = world.Resource<mir::PostProcessingSettings>().effects;
            effects.clear();
            effects["Anti-Aliasing"] = { true,{ { "mode", mode == "dlss" ? "dlss" : "off" } } };
            if (scenario == "effects")
            {
                effects["DepthOfField"] = { true,{ { "focusDistance", 8.0f } } };
                effects["MotionBlur"] = { true, nlohmann::json::object() };
                effects["Bloom"] = { true, nlohmann::json::object() };
            }
            MIR_LOG_INFO("Particle probe: mode={}, scenario={}, 960x540, 90 frames, validation on", mode, scenario);
            if (unavailable)
                MIR_LOG_WARN("Requested RT/RR unavailable: this run is NOT proof of that mode");
            // Always start/stop the App scheduler, including on unavailable-mode runs.
            app.Run();
            renderer.SetParticleCompositionProbe(std::nullopt);
            if (unavailable)
                return 77;
        }
        mir::FlushLog();
        return mir::ErrorCount() == 0 ? 0 : 1;
    }
    catch (const std::exception &error)
    {
        MIR_LOG_ERROR("Particle composition fixture failed: {}", error.what());
        return 1;
    }
}
