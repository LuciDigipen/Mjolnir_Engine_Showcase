/**
 * @file main.cpp
 * @author sumin.park
 * @brief Device check that two Worlds share one prepared model through unload, reload and close.
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
#include <core/fs/FileIO.h>
#include <core/log/Log.h>
#include <jobs/JobExecutor.h>
#include <renderer/MeshResolveSystem.h>
#include <renderer/VulkanRenderer.h>
#include <renderer/CameraMath.h>
#include <renderer/Shapes.h>
#include <window/Window.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <vector>
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

    void CheckSettledGeometry(mir::VulkanRenderer &renderer, mir::Window &window)
    {
        if (!renderer.RayTracingSupported()) return;
        const auto cube = mir::MakeCube();
        const auto mesh = renderer.CreateMesh(cube.vertices, cube.indices);
        Require(!mesh.IsNull(), "Pose cache fixture upload failed");
        mir::MeshPrimitive primitive;
        primitive.vertices.resize(cube.vertices.size());
        primitive.morphTargets.resize(1);
        auto &morph = primitive.morphTargets[0];
        morph.positions.assign(cube.vertices.size(), glm::vec3(0, .5f, 0));
        morph.normals.assign(cube.vertices.size(), glm::vec3(0));
        morph.tangents.assign(cube.vertices.size(), glm::vec3(0));
        const float zero = 0, one = 1;
        const auto down = mir::BuildDeformationData(primitive, std::span(&zero, 1), {});
        const auto up = mir::BuildDeformationData(primitive, std::span(&one, 1), {});
        mir::DrawItem item;
        item.mesh = mesh;
        item.model[3].z = -3;
        item.previousModel = item.model;
        item.settledDeformation = true;
        item.deformation = down;
        item.previousDeformation = down;
        // A second aircraft shares this primitive and settled geometry.
        mir::DrawItem other = item;
        other.model[3].x = -1;
        std::vector<mir::DrawItem> items{item, other};
        mir::RenderView view;
        view.items = items;
        view.rayTracingItems = items;
        view.viewProj = mir::MakeViewProjection(glm::mat4(1), mir::Camera{}, 640.f / 360.f);
        view.nearZ = .1f;
        view.farZ = 100;
        view.fovYDegrees = 60;
        const auto draw = [&](int frames) {
            items[0] = item;
            for (int i = 0; i < frames; ++i)
            {
                window.PollEvents();
                renderer.TraceFrame(std::span(&view, 1));
            }
        };
        draw(4);
        const auto warm = renderer.RayTracingDeformations();
        Require(warm.settledBuilds == 2, "Settled instances did not share one BLAS per frame slot");
        item.model[3].x = 1; // Translation must not rebuild local geometry.
        draw(6);
        Require(renderer.RayTracingDeformations().blasBuilds == warm.blasBuilds,
            "A settled or moving instance rebuilt its BLAS");
        item.deformation = up;
        // Preserve down geometry for velocity on the first paused frame.
        const auto beforeTransition = renderer.RayTracingDeformations();
        draw(1);
        Require(renderer.RayTracingDeformations().settledBuilds == beforeTransition.settledBuilds,
            "A pose transition lost its previous deformation");
        item.previousDeformation = up;
        draw(4);
        const auto both = renderer.RayTracingDeformations();
        item.deformation = down;
        item.previousDeformation = down;
        draw(6);
        Require(renderer.RayTracingDeformations().blasBuilds == both.blasBuilds,
            "Returning to the other settled pose rebuilt its BLAS");
        item.settledDeformation = false;
        draw(4);
        Require(renderer.RayTracingDeformations().blasBuilds >= both.blasBuilds + 4,
            "Animated samples did not use dynamic BLAS builds");
        item.settledDeformation = true;
        draw(4);
        const auto resumed = renderer.RayTracingDeformations();
        Require(resumed.settledBuilds == both.settledBuilds,
            "Animation evicted a settled endpoint");
        // A replacement mesh must not reuse BLASes of a retired generation.
        renderer.DestroyMesh(mesh);
        const auto replacement = renderer.CreateMesh(cube.vertices, cube.indices);
        item.mesh = replacement;
        items[1].mesh = replacement;
        draw(4);
        Require(renderer.RayTracingDeformations().settledBuilds >= resumed.settledBuilds + 2,
            "A replacement mesh reused stale settled geometry");
        renderer.DestroyMesh(replacement);
        MIR_LOG_INFO("Settled geometry GPU check passed: {} builds, {} cache hits",
            resumed.blasBuilds, resumed.settledHits);
    }

    // Reloads change only this private fixture, never the build's cooked files.
    struct CookedFixture
    {
        struct Entry
        {
            mir::AssetManifestEntry metadata;
            std::string path, logical;
            std::vector<mir::AssetId> dependencies;
        };

        CookedFixture(const std::filesystem::path &source, const std::string &prefix, mir::AssetId model)
            : path(std::filesystem::absolute(prefix + "-assets-" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count())))
        {
            const auto manifest = mir::AssetManifest::LoadFile(source / mir::kAssetManifestFileName);
            Require(manifest.has_value(), "Cannot read the source manifest");
            std::filesystem::create_directories(path);
            for (const auto id : manifest->Closure(model))
            {
                const auto &entry = *manifest->Find(id);
                entries.push_back({ entry, std::string(manifest->PathOf(entry)),
                    std::string(manifest->LogicalPathOf(entry)), {} });
                const auto dependencies = manifest->Dependencies(entry);
                entries.back().dependencies.assign(dependencies.begin(), dependencies.end());
                const auto destination = path / entries.back().path;
                std::filesystem::create_directories(destination.parent_path());
                std::filesystem::copy_file(source / entries.back().path, destination);
            }
            Publish();
        }

        ~CookedFixture()
        {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }

        void Publish()
        {
            std::vector<mir::AssetManifestSourceEntry> sources;
            for (const auto &entry : entries)
                sources.push_back({ entry.metadata, entry.path, entry.logical, entry.dependencies });
            Require(mir::WriteBytes(path / mir::kAssetManifestFileName, mir::BuildAssetManifestBlob(sources)),
                "Cannot write the fixture manifest");
        }

        template <class T, class Change> size_t Replace(uint32_t tag, Change change)
        {
            const auto found = std::find_if(entries.begin(), entries.end(),
                [tag](const Entry &entry) { return entry.metadata.typeTag == tag; });
            Require(found != entries.end(), "Missing reload fixture dependency");
            const auto bytes = mir::ReadBytes(path / found->path);
            Require(bytes.has_value(), "Cannot read reload fixture dependency");
            const auto blob = mir::ParseAssetBlob(*bytes);
            Require(blob.has_value(), "Invalid reload fixture blob");
            T asset;
            std::string error;
            Require(mir::ReadTypedAsset(*blob, asset, error), "Cannot decode fixture: " + error);
            const size_t affected = change(asset);
            const auto replacement = mir::BuildTypedAsset(asset);
            const auto view = mir::ParseAssetBlob(replacement);
            Require(view.has_value(), "Cannot encode reload fixture");
            found->path = std::format("{:016x}-{:016x}.blob", found->metadata.id.value, view->header.contentHash);
            Require(mir::WriteBytes(path / found->path, replacement), "Cannot write changed fixture");
            Publish();
            return affected;
        }

        std::filesystem::path path;
        std::vector<Entry> entries;
    };

    struct Handles
    {
        std::vector<mir::MeshHandle> meshes;
        std::vector<mir::MaterialHandle> materials;
        std::vector<mir::TextureHandle> textures;
        bool operator==(const Handles &) const = default;
    };

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
        CheckSettledGeometry(renderer, *window);

        const mir::AssetId model{ 0x574f8c6b27fedda9ull };
        // the App's one library, declared after the renderer so it goes first
        const auto library = std::make_shared<mir::ModelLibrary>(renderer, renderer, renderer.ErrorTexture());
        CookedFixture fixture(cooked, prefix, model);
        Rig rig(fixture.path);
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

        const auto handles = [&](PreviewWorld &world)
        {
            Handles result;
            for (const auto &draw : world.mWorld.Resource<mir::ModelDrawItems>().items)
            {
                result.meshes.push_back(draw.mesh);
                result.materials.push_back(draw.material);
            }
            const auto entry = library->Acquire(model, &rig.mService, "reload check");
            Require(entry->ready != nullptr, "Replacement did not publish a ready model");
            for (const auto &texture : entry->ready->textures)
            {
                Require(texture.Get() != nullptr, "Prepared model lost its texture claim");
                result.textures.push_back(texture.Get()->texture);
            }
            return result;
        };
        const auto initialHandles = handles(*scene);
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
        Require(renderer.MeshStreaming().textureQueued == first.textureQueued &&
            handles(*scene) == initialHandles && handles(*preview) == initialHandles,
            "An identical cook replaced shared handles or uploaded textures");

        const auto reload = [&]
        {
            rig.mAssets.BeginReload();
            rig.mAssets.CommitReload();
            scene->mModels->CommitReload();
            preview->mModels->CommitReload();
        };
        const auto awaitReplacement = [&](const Handles &old, bool meshChanged)
        {
            for (int frame = 0; frame < 600; ++frame)
            {
                rig.Step(renderer, scene.get(), preview.get());
                Require(scene->Draws() == drawCount && preview->Draws() == drawCount,
                    "Changed-content reload interrupted drawing");
                const auto &sceneDraws = scene->mWorld.Resource<mir::ModelDrawItems>().items;
                const auto &previewDraws = preview->mWorld.Resource<mir::ModelDrawItems>().items;
                bool changed = false;
                for (size_t i = 0; i < sceneDraws.size(); ++i)
                {
                    Require(renderer.MeshState(sceneDraws[i].mesh) == mir::MeshUploadState::Ready &&
                        renderer.MeshState(previewDraws[i].mesh) == mir::MeshUploadState::Ready,
                        "Reload exposed an unfinished mesh");
                    changed |= meshChanged ? sceneDraws[i].mesh != old.meshes[i]
                                           : sceneDraws[i].material != old.materials[i];
                }
                if (changed)
                {
                    const auto current = handles(*scene);
                    Require(current == handles(*preview), "Scene and preview published different replacements");
                    return current;
                }
            }
            throw std::runtime_error("Changed-content replacement never became visible");
        };

        const auto original = handles(*scene);
        const auto beforeMesh = renderer.MeshStreaming();
        const size_t changedPrimitives = fixture.Replace<mir::MeshAsset>(mir::kMeshAssetTypeTag,
            [](mir::MeshAsset &mesh)
            {
                for (auto &primitive : mesh.primitives)
                    for (auto &vertex : primitive.vertices)
                        vertex.position.x += 0.25f;
                return mesh.primitives.size();
            });
        reload();
        const auto changedMesh = awaitReplacement(original, true);
        Require(changedMesh.meshes != original.meshes, "Changed mesh retained its old handles");
        Require(changedMesh.textures == original.textures && changedMesh.materials == original.materials,
            "Mesh-only reload replaced unchanged textures or materials");
        Require(renderer.MeshStreaming().queued == beforeMesh.queued + changedPrimitives &&
            renderer.MeshStreaming().textureQueued == beforeMesh.textureQueued,
            "Mesh-only reload uploaded more than the changed primitives");

        const auto beforeTexture = renderer.MeshStreaming();
        fixture.Replace<mir::TextureAsset>(mir::kTextureAssetTypeTag,
            [](mir::TextureAsset &texture)
            {
                Require(!texture.rgba.empty(), "Texture fixture has no pixels");
                texture.rgba[0] ^= 0x7f;
                return size_t{ 1 };
            });
        reload();
        const auto changedTexture = awaitReplacement(changedMesh, false);
        Require(changedTexture.meshes == changedMesh.meshes, "Texture-only reload replaced unchanged meshes");
        Require(changedTexture.textures != changedMesh.textures && changedTexture.materials != changedMesh.materials,
            "Texture-only reload did not replace texture and material handles");
        Require(renderer.MeshStreaming().queued == beforeTexture.queued &&
            renderer.MeshStreaming().textureQueued == beforeTexture.textureQueued + 1,
            "Texture-only reload uploaded more than the changed texture");

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
    return mir::ErrorCount() == 0 ? 0 : 1;
}
