/**
 * @file main.cpp
 * @author sumin.park
 * @brief Device check that two Worlds share one prepared model through unload, reload and close.
 *
 * A scene World and a prefab-preview style World, built the way PrefabWorkspace builds its previews, both
 * show the HelloWorld model. The check follows the model through the first load, the scene unloading
 * while the preview keeps drawing, an identical cook, a cook with changed content, and the preview closing.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <animation/ModelLibrary.h>
#include <animation/ModelPlayer.h>
#include <animation/ModelSystem.h>
#include <assets/AssetResolver.h>
#include <assets/AssetsModule.h>
#include <assets/AsyncAssetService.h>
#include <core/ecs/CommandBuffer.h>
#include <core/ecs/CoreModule.h>
#include <core/ecs/EngineMode.h>
#include <core/ecs/SystemScheduler.h>
#include <core/ecs/TransformHierarchy.h>
#include <core/ecs/World.h>
#include <core/log/Log.h>
#include <jobs/JobExecutor.h>
#include <renderer/MeshResolveSystem.h>
#include <renderer/VulkanRenderer.h>
#include <window/Window.h>

#include <filesystem>
#include <format>
#include <stdexcept>
#include <string>

namespace
{
    void Require(bool condition, const std::string &message)
    {
        if (!condition)
            throw std::runtime_error(message);
    }

    // What PrefabWorkspace gives a preview: its own World, scheduler and the two resolve systems.
    struct PreviewWorld
    {
        PreviewWorld(mir::VulkanRenderer &renderer, std::shared_ptr<mir::ModelLibrary> library,
            mir::AsyncAssetService &service, const mir::AssetResolver &resolver, mir::AssetId model)
        {
            mWorld.EmplaceResource<mir::EngineModeState>();
            mWorld.EmplaceResource<mir::ComponentFieldListeners>();
            mWorld.EmplaceResource<mir::ModelDrawItems>();
            mWorld.EmplaceResource<mir::ModelMorphOverrides>();
            mScheduler.AddSystem<mir::TransformPropagateSystem>(mir::SystemPhase::Propagate,
                mir::SystemOrder{ mir::SystemOrder::kNormal });
            mScheduler.AddSystem<mir::MeshResolveSystem>(mir::SystemPhase::LateUpdate,
                mir::SystemOrder{ mir::SystemOrder::kNormal }, renderer, &service, resolver);
            mModels = &mScheduler.AddSystem<mir::ModelSystem>(mir::SystemPhase::LateUpdate,
                mir::SystemOrder{ mir::SystemOrder::kNormal }, std::move(library), &service, resolver);
            mHooks = mWorld.SetComponentHooks<mir::ModelPlayer>({ .onRemove =
                [this](mir::World &world, mir::Entity entity) { mModels->Forget(world, entity); } });
            mir::SystemContext context{ mWorld, mCommands };
            mScheduler.Start(context);

            const mir::Entity entity = mWorld.CreateEntity();
            mir::AddTransform(mWorld, entity);
            mir::ModelPlayer player;
            player.SetModelAsset(model);
            mWorld.AddComponent<mir::ModelPlayer>(entity, std::move(player));
        }

        ~PreviewWorld()
        {
            mir::SystemContext context{ mWorld, mCommands };
            mScheduler.Stop(context);
        }

        void Frame()
        {
            mir::SystemContext context{ mWorld, mCommands, 0.016f, 0.0, ++mFrame };
            mScheduler.Update(context);
        }

        size_t Draws() { return mWorld.Resource<mir::ModelDrawItems>().items.size(); }

        mir::CommandBuffer mCommands;
        mir::World mWorld;
        mir::SystemScheduler mScheduler;
        mir::ModelSystem *mModels = nullptr;
        mir::Registration mHooks;
        uint64_t mFrame = 0;
    };

    struct Rig
    {
        Rig(const std::filesystem::path &cooked)
            : mAssets(cooked), mJobs(2), mService(mAssets, mJobs, 2), mResolver(mAssets, "games/HelloWorld")
        {}

        void Step(mir::VulkanRenderer &renderer, PreviewWorld *a, PreviewWorld *b)
        {
            mService.DrainCompletions();
            if (a)
                a->Frame();
            if (b)
                b->Frame();
            renderer.DrawFrame({});
        }

        mir::AssetsModule mAssets;
        mir::JobExecutor mJobs;
        mir::AsyncAssetService mService;
        mir::AssetResolver mResolver;
    };
}

int main(int argc, char **argv)
{
    const std::string prefix = argc > 1 ? argv[1] : "model-worlds-gpu";
    const std::filesystem::path cooked = argc > 2 ? argv[2] : "cooked";
    mir::LogConfig logging;
    logging.filePath = prefix + ".log";
    mir::InitLog(logging);
    try
    {
        mir::RegisterCoreComponents();
        mir::RegisterModelPlayer();
        auto window = mir::Window::Create({ .mWidth = 640, .mHeight = 360, .mTitle = "Model worlds GPU check",
            .mStartHidden = true });
        mir::VulkanRenderer renderer;
        struct Shutdown
        {
            mir::VulkanRenderer &mRenderer;
            ~Shutdown() { mRenderer.Shutdown(); }
        } shutdown{ renderer };
        Require(renderer.Initialize({ window.get(), "ModelWorldsGpuCheck", true }), "Renderer initialization failed");

        const mir::AssetId model{ 0x574f8c6b27fedda9ull };
        // the App's one library, declared after the renderer so it goes first
        const auto library = std::make_shared<mir::ModelLibrary>(renderer, renderer, renderer.ErrorTexture());
        Rig rig(cooked);
        Require(rig.mAssets.Manifest().Find(model) != nullptr, "The cooked assets have no HelloWorld model");

        auto scene = std::make_unique<PreviewWorld>(renderer, library, rig.mService, rig.mResolver, model);
        auto preview = std::make_unique<PreviewWorld>(renderer, library, rig.mService, rig.mResolver, model);
        const auto settle = [&](PreviewWorld *a, PreviewWorld *b, const char *what)
        {
            for (int frame = 0; frame < 600; ++frame)
            {
                rig.Step(renderer, a, b);
                if ((a == nullptr || a->Draws() > 0) && (b == nullptr || b->Draws() > 0))
                    return;
            }
            throw std::runtime_error(std::string("Never drew: ") + what);
        };

        settle(scene.get(), preview.get(), "first load");
        const auto first = renderer.MeshStreaming();
        const size_t drawCount = scene->Draws();
        Require(drawCount == preview->Draws(), "The two Worlds draw different numbers of primitives");
        Require(first.queued == drawCount || first.queued > 0, "No mesh was queued");
        Require(first.queued <= drawCount, "The shared model's meshes were queued once per World");
        Require(first.textureQueued <= 1, "The shared model's texture was queued once per World");

        scene->mModels->ReleaseResources();
        scene.reset();
        for (int frame = 0; frame < 30; ++frame)
        {
            rig.Step(renderer, nullptr, preview.get());
            Require(preview->Draws() == drawCount, "The preview stopped drawing after the scene unloaded");
        }
        Require(renderer.MeshStreaming().queued == first.queued, "A scene unload queued meshes again");

        scene = std::make_unique<PreviewWorld>(renderer, library, rig.mService, rig.mResolver, model);
        settle(scene.get(), preview.get(), "scene reload");
        Require(renderer.MeshStreaming().queued == first.queued, "Reloading the scene uploaded the model again");

        rig.mAssets.BeginReload();
        rig.mAssets.CommitReload();
        scene->mModels->CommitReload();
        preview->mModels->CommitReload();
        for (int frame = 0; frame < 60; ++frame)
        {
            rig.Step(renderer, scene.get(), preview.get());
            Require(scene->Draws() == drawCount && preview->Draws() == drawCount, "A reload interrupted the drawing");
        }
        Require(renderer.MeshStreaming().queued == first.queued, "An identical cook uploaded the model again");

        preview->mModels->ReleaseResources();
        preview.reset();
        for (int frame = 0; frame < 30; ++frame)
        {
            rig.Step(renderer, scene.get(), nullptr);
            Require(scene->Draws() == drawCount, "The scene stopped drawing after the preview closed");
        }
        scene->mModels->ReleaseResources();
        scene.reset();
        for (int frame = 0; frame < 10; ++frame)
            rig.Step(renderer, nullptr, nullptr);
        library->ReleaseAll();
        renderer.ReleaseSceneResources();
        MIR_LOG_INFO("Model worlds check passed with {} primitives", drawCount);
    }
    catch (const std::exception &error)
    {
        MIR_LOG_ERROR("Model worlds check failed: {}", error.what());
        return 1;
    }
    return 0;
}
