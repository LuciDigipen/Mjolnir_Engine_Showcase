/**
 * @file VulkanRenderer.cpp
 * @author Sumin Park
 * @brief Renderer initialization, shutdown and scene/model release
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer/SurfaceMaterialCatalog.h>
#include <renderer/VulkanRenderer.h>
#include "vulkan/CameraBuffer.h"
#include "vulkan/DebugDraw.h"
#include "vulkan/Device.h"
#include "vulkan/FrameCapture.h"
#include "vulkan/FrameSync.h"
#include "vulkan/FxCompositionPass.h"
#include "vulkan/ImGuiOverlay.h"
#include "vulkan/MaterialStore.h"
#include "vulkan/MaterialTable.h"
#include "vulkan/MeshUploader.h"
#include "vulkan/ModelDescriptors.h"
#include "vulkan/PostProcessingStack.h"
#include "vulkan/RayTracer.h"
#include "vulkan/RasterSurfaces.h"
#include "vulkan/SsgiTrace.h"
#include "vulkan/ResourceStore.h"
#include "vulkan/ShadowMaps.h"
#include "vulkan/Swapchain.h"
#include "vulkan/VulkanConstants.h"
#if MIR_WITH_RTXGI
#include "vulkan/Ddgi.h"
#endif

#include <core/fs/FileIO.h>
#include <core/fs/Paths.h>
#include <core/log/Log.h>
#include <core/log/SetupError.h>

#include <string>
#include <utility>

namespace mir
{
    VulkanRenderer::VulkanRenderer()
        : mDevice(std::make_unique<vk::Device>()),
        mSwapchain(std::make_unique<vk::Swapchain>()),
        mFrameSync(std::make_unique<vk::FrameSync>()),
        mResources(std::make_unique<vk::ResourceStore>()),
        mMaterialTable(std::make_unique<vk::MaterialTable>()),
        mMaterialStore(std::make_unique<vk::MaterialStore>()),
        mRasterSurfaces(std::make_unique<vk::RasterSurfaces>()),
        mSsgiTrace(std::make_unique<vk::SsgiTrace>()),
        mCameraBuffer(std::make_unique<vk::CameraBuffer>()),
        mShadowMaps(std::make_unique<vk::ShadowMaps>()),
        mModelDescriptors(std::make_unique<vk::ModelDescriptors>()),
        mRayTracer(std::make_unique<vk::RayTracer>()),
#if MIR_WITH_RTXGI
        mDdgi(std::make_unique<vk::Ddgi>()),
#endif
        mPostProcessing(std::make_unique<vk::PostProcessingStack>()),
        mFxComposition(std::make_unique<vk::FxCompositionPass>()),
        mCapture(std::make_unique<vk::FrameCapture>()),
        mImGuiOverlay(std::make_unique<vk::ImGuiOverlay>()),
        mDebugDraw(std::make_unique<vk::DebugDraw>()),
        mMeshUploads(std::make_unique<vk::MeshUploader>())
    {}
    VulkanRenderer::~VulkanRenderer() = default;

    bool VulkanRenderer::RayTracingSupported() const { return mDevice->RayTracingSupported(); }

    bool VulkanRenderer::RayTracingActive(bool requested) const
    {
        return requested && RayTracingSupported() && !mRayTracingOutOfMemory;
    }

    bool VulkanRenderer::WireframeAvailable() const { return mDebugDraw->WireframeAvailable(); }

    bool VulkanRenderer::DlssAvailable() const { return mPostProcessing->DlssAvailable(); }

    FxDeviceContext VulkanRenderer::FxDevice() const
    {
        return {mDevice->Handle(), mDevice->Allocator(), mDevice->ValidationEnabled(),
            mDevice->Properties().limits.timestampPeriod, mDevice->TimestampBits()};
    }

    Registration VulkanRenderer::RegisterFxSystem(IFxSystem &system)
    {
        if (mFxSystem)
            throw SetupError(ErrorCategory::Internal, "Register FX system", "renderer",
                "An FX provider is already registered", "Unregister the existing provider before registering another.");
        mFxSystem = &system;
        return Registration(mFxLifetime, [this, provider = &system]
        {
            if (mFxSystem != provider) return;
            if (mDevice->Handle()) mDevice->WaitIdle();
            provider->Shutdown();
            mFxSystem = nullptr;
        });
    }

    VulkanRenderer::GpuMemoryUsage VulkanRenderer::GetGpuMemoryUsage() const
    {
        const vk::Device::MemoryBudget budget = mDevice->GpuMemoryBudget();
        return GpuMemoryUsage{ .mUsageBytes = budget.mUsageBytes, .mBudgetBytes = budget.mBudgetBytes };
    }

    VkExtent2D VulkanRenderer::SwapchainExtent() const { return mSwapchain->Extent(); }

    void VulkanRenderer::RequestSwapchainRecreate() { mSwapchain->MarkOutOfDate(); }

    uint64_t VulkanRenderer::FxCompositionMemoryBytes() const { return mFxComposition->MemoryBytes(); }

    bool VulkanRenderer::Initialize(const RendererDesc &desc)
    {
        if (desc.window == nullptr)
        {
            MIR_LOG_ERROR("Initialize: no window");
            return false;
        }

        mWindow = desc.window;

        ShaderGenerationStore generationStore(ExecutableDir() / "shaders");
        if (const auto current = generationStore.ReadCurrent(generationStore.Root()))
            SetShaderRootOverride(generationStore.GenerationPath(current->mId));

        if (!mDevice->Initialize(desc))
            return false;

        if (!mSwapchain->Initialize(*mDevice, *mWindow))
            return false;

        if (!mFrameSync->Initialize(*mDevice))
            return false;

        if (!mCapture->Initialize(*mDevice))
            return false;

        if (!mResources->Initialize(*mDevice))
            return false;

        vk::MaterialTable *materialTable = nullptr;
        if (mDevice->SharedMaterialTablesSupported())
        {
            if (!mMaterialTable->Initialize(*mDevice))
                return false;
            materialTable = mMaterialTable.get();
        }
        else
            mMaterialTable.reset();

        if (!mCameraBuffer->Initialize(*mDevice))
            return false;

        if (!mShadowMaps->Initialize(*mDevice))
            return false;

        if (!mModelDescriptors->Initialize(*mDevice))
            return false;

        if (!mMaterialStore->Initialize(*mDevice, *mResources, materialTable, vk::kHdrColorFormat,
            { mCameraBuffer->SetLayout(), mModelDescriptors->SetLayout(), mShadowMaps->SetLayout() }))
            return false;

        if (mDevice->RayTracingSupported() &&
            !mRayTracer->Initialize(*mDevice, mSwapchain->Extent(), mCameraBuffer->SetLayout(), *mMaterialTable,
                mMaterialStore->SurfaceMaterials().materials))
            return false;

        if (!mMeshUploads->Initialize(*mDevice))
            return false;
        mRasterSurfaces->Initialize(*mDevice);
        mSsgiTrace->Initialize(*mDevice, mCameraBuffer->SetLayout());
#if MIR_WITH_RTXGI
        if (mDevice->RayTracingSupported() && mDevice->DdgiSupported())
            mDdgi->Initialize(*mDevice, mRayTracer->SceneLayout(), mCameraBuffer->SetLayout());
#endif

        if (!mPostProcessing->Initialize(*mDevice, mSwapchain->Format(), mSwapchain->Extent(),
            mCameraBuffer->SetLayout()))
            return false;

        if (!mDebugDraw->Initialize(*mDevice, mSwapchain->Format()))
            return false;
        InitializeShaderLibrary();
        return true;
    }

    namespace
    {
        std::string ShaderOutputName(std::string_view path)
        {
            return std::filesystem::path(path).filename().string();
        }

        const ShaderManifestOutput *FindManifestOutput(const ShaderManifest &manifest, std::string_view name)
        {
            return manifest.Find(name);
        }

        void AddOutputs(std::vector<std::string> &outputs, std::string_view stem,
            std::initializer_list<std::string_view> entries)
        {
            for (const std::string_view entry : entries)
            {
                outputs.push_back(std::string(stem) + "." + std::string(entry) + ".spv");
            }
        }
    }

    void VulkanRenderer::RegisterShaderConsumers(ShaderLibrary &library,
        const SurfaceMaterialCatalog *catalog)
    {
        library.Clear();

        std::vector<std::string> materialVertex;
        std::vector<std::string> materialFragment;
        for (const char *stem : { "default", "unlit", "error", "model", "sky" })
        {
            materialVertex.push_back(std::string(stem) + ".vertexMain.spv");
            materialFragment.push_back(std::string(stem) + ".fragmentMain.spv");
        }
        // The merged opaque pass uses the built-in shaders' surface frontends.
        for (const char *stem : { "default", "model", "unlit", "error", "sky" })
        {
            materialVertex.push_back(std::string(stem) + "Surface.vertexMain.spv");
            materialFragment.push_back(std::string(stem) + "Surface.fragmentMain.spv");
        }
        std::vector<std::string> surfaceClosestHit;
        const SurfaceMaterialCatalog *materials = catalog != nullptr ? catalog :
            &mMaterialStore->SurfaceMaterials();
        for (const SurfaceMaterialRegistration &registration : materials->materials)
        {
            materialVertex.push_back(registration.stem + ".vertexMain.spv");
            materialFragment.push_back(registration.stem + ".fragmentMain.spv");
            materialFragment.push_back(registration.stem + ".surface.fragmentMain.spv");
            surfaceClosestHit.push_back(registration.stem + ".closestHitSurface.spv");
        }
        library.Register("MaterialStore.vertex", std::move(materialVertex), { "vertex" });
        library.Register("MaterialStore.fragment", std::move(materialFragment), { "fragment" });

        std::vector<std::string> bloom;
        AddOutputs(bloom, "bloom", { "vertexMain", "brightpassMain", "downsampleMain", "upsampleMain" });
        library.Register("Bloom", std::move(bloom), { "vertex", "fragment" });

        std::vector<std::string> dof;
        AddOutputs(dof, "depthOfField", { "vertexMain", "cocMain", "nearBlurMain", "farBlurMain", "compositeMain" });
        library.Register("DepthOfField", std::move(dof), { "vertex", "fragment" });

        std::vector<std::string> fxaa;
        AddOutputs(fxaa, "fxaa", { "vertexMain", "fragmentMain" });
        library.Register("Fxaa", std::move(fxaa), { "vertex", "fragment" });

        std::vector<std::string> motionBlur;
        AddOutputs(motionBlur, "motionBlur", { "vertexMain", "compositeMain" });
        library.Register("MotionBlur", std::move(motionBlur), { "vertex", "fragment" });

        std::vector<std::string> tonemap;
        AddOutputs(tonemap, "tonemap", { "vertexMain", "fragmentMain" });
        library.Register("PostProcess", std::move(tonemap), { "vertex", "fragment" });

        std::vector<std::string> velocity;
        AddOutputs(velocity, "velocity", { "vertexMain", "fragmentMain" });
        library.Register("VelocityPass", std::move(velocity), { "vertex", "fragment" });

        library.Register("ShadowMaps.depth", { "shadowDepth.vertexMain.spv" }, { "vertex" });
        library.Register("ShadowMaps.distance",
            { "shadowDistance.vertexMain.spv", "shadowDistance.fragmentMain.spv" }, { "vertex", "fragment" });

        library.Register("RayTracer.raygen", { "raytrace.rayGenMain.spv" }, { "raygeneration" });
        library.Register("RayTracer.miss", { "raytrace.missMain.spv", "raytrace.shadowMissMain.spv" },
            { "miss" });
        std::vector<std::string> rayHit{ "raytrace.closestHitMain.spv" };
        rayHit.insert(rayHit.end(), surfaceClosestHit.begin(), surfaceClosestHit.end());
        library.Register("RayTracer.closesthit", std::move(rayHit), { "closesthit" });
        library.Register("RayTracer.anyhit", { "raytrace.anyHitMain.spv", "raytrace.shadowAnyHitMain.spv" },
            { "anyhit" });
    }

    bool VulkanRenderer::LoadAcceptedShaderSignatures(std::string *error)
    {
        const std::optional<ShaderManifest> manifest =
            LoadShaderManifest(ShaderPath("shader_manifest.json"), error);
        if (!manifest)
            return false;
        if (!mShaderLibrary.ValidateCoverage(*manifest, error))
            return false;

        mAcceptedShaderHashes.clear();
        for (const ShaderManifestOutput &output : manifest->outputs)
        {
            const std::string name = ShaderOutputName(output.output);
            const auto bytes = ReadBytes(ShaderPath(name));
            if (!bytes)
            {
                if (error != nullptr) *error = "shader output is missing: " + name;
                return false;
            }
            mAcceptedShaderHashes[name] = HashShaderContent(*bytes);
        }

        for (const std::string &name : mShaderLibrary.Outputs())
        {
            const ShaderManifestOutput *output = FindManifestOutput(*manifest, name);
            if (output == nullptr)
            {
                if (error != nullptr)
                    *error = "manifest output is missing for '" + name + "'";
                return false;
            }
            const std::optional<std::vector<std::byte>> spirv = ReadBytes(ShaderPath(name));
            if (!spirv)
            {
                if (error != nullptr)
                    *error = "accepted shader output is missing for '" + name + "'";
                return false;
            }
            const auto abi = ParseSpirvAbi(*spirv, output->entry, error);
            if (!abi)
                return false;
            if (abi->stage != output->stage)
            {
                if (error != nullptr)
                    *error = "'" + name + "' compiles as " + abi->stage + " but the manifest says " +
                        output->stage;
                return false;
            }
            mAcceptedSpirvAbi[name] = *abi;
        }
        return true;
    }

    void VulkanRenderer::InitializeShaderLibrary()
    {
        RegisterShaderConsumers(mShaderLibrary, nullptr);
        std::string error;
        mShaderReloadReady = LoadAcceptedShaderSignatures(&error);
        if (!mShaderReloadReady)
            MIR_LOG_WARN("Shader hot reload is unavailable: {}", error);
    }

    bool VulkanRenderer::PrepareShaderReload(const std::filesystem::path &generationDirectory,
        std::string *error)
    {
        if (!mShaderReloadReady)
        {
            if (error != nullptr)
                *error = "shader hot reload is unavailable";
            return false;
        }
        if (mShaderReloadPrepared)
        {
            if (error != nullptr)
                *error = "a shader reload is already prepared";
            return false;
        }

        struct RootScope
        {
            std::filesystem::path previous;
            ~RootScope() { SetShaderRootOverride(std::move(previous)); }
        } rootScope{ShaderRootOverride()};
        try
        {
        SetShaderRootOverride(generationDirectory);

        const auto fail = [&](std::string message)
            {
                if (error != nullptr)
                    *error = std::move(message);
                mPendingShaderReload.DiscardAll();
                return false;
            };

        std::string manifestError;
        const std::optional<ShaderManifest> manifest =
            LoadShaderManifest(ShaderPath("shader_manifest.json"), &manifestError);
        if (!manifest)
            return fail(manifestError);

        std::string catalogError;
        const std::optional<SurfaceMaterialCatalog> candidateCatalog =
            LoadSurfaceMaterialCatalog(&catalogError);
        if (!candidateCatalog)
            return fail(catalogError);
        mPendingCatalog = *candidateCatalog;

        ShaderLibrary candidateLibrary;
        RegisterShaderConsumers(candidateLibrary, &mPendingCatalog);
        std::string coverageError;
        if (!candidateLibrary.ValidateCoverage(*manifest, &coverageError))
            return fail(coverageError);

        std::map<std::string, uint64_t> candidateHashes;
        std::vector<std::string> managed = candidateLibrary.Outputs();
        for (const ShaderManifestOutput &output : manifest->outputs)
        {
            const std::string name = ShaderOutputName(output.output);
            const auto bytes = ReadBytes(ShaderPath(name));
            if (!bytes) return fail("shader output is missing: " + name);
            candidateHashes[name] = HashShaderContent(*bytes);
            if (output.type == "surface" && output.entry == "layoutMain")
                managed.push_back(name); // Reflection-only output, no runtime pipeline.
        }
        const ShaderCompatibility ownership = ValidateShaderOwnerCoverage(managed, mAcceptedShaderHashes,
            candidateHashes);
        if (!ownership.compatible)
            return fail(ownership.differences.front());

        std::map<std::string, ShaderSpirvAbi> candidates;
        for (const std::string &name : candidateLibrary.Outputs())
        {
            const ShaderManifestOutput *output = FindManifestOutput(*manifest, name);
            if (output == nullptr)
                return fail("manifest output is missing for '" + name + "'");

            const std::optional<std::vector<std::byte>> spirv = ReadBytes(ShaderPath(name));
            if (!spirv || !ValidateSpirvModule(*spirv, error))
                return fail("shader output is missing: " + name);
            const auto abi = ParseSpirvAbi(*spirv, output->entry, error);
            if (!abi)
                return fail("shader output has no usable ABI: " + name);
            if (abi->stage != output->stage)
                return fail("'" + name + "' compiles as " + abi->stage + " but the manifest says " +
                    output->stage);

            const auto accepted = mAcceptedSpirvAbi.find(name);
            if (accepted != mAcceptedSpirvAbi.end())
            {
                const ShaderCompatibility compatibility = CompareSpirvAbi(accepted->second, *abi);
                if (!compatibility.compatible)
                {
                    std::string message = "a restart is required for '" + name + "'";
                    for (const std::string &difference : compatibility.differences)
                        message += "; " + difference;
                    return fail(message);
                }
            }
            candidates[name] = *abi;
        }

        mDevice->WaitIdle();
        ++mShaderReloadStats.mDeviceIdles;

        mPendingShaderReload.Clear();
        mPendingShaderReload.AddStage("materials",
            [&](std::string &stageError)
            { return mMaterialStore->PrepareShaderReload(mPendingCatalog, &stageError); },
            [&] { mMaterialStore->CommitShaderReload(); },
            [&] { mMaterialStore->DiscardShaderReload(); });
        mPendingShaderReload.AddStage("shadows",
            [&](std::string &stageError) { return mShadowMaps->PrepareShaderReload(&stageError); },
            [&] { mShadowMaps->CommitShaderReload(); },
            [&] { mShadowMaps->DiscardShaderReload(); });
        mPendingShaderReload.AddStage("post",
            [&](std::string &stageError) { return mPostProcessing->PrepareShaderReload(&stageError); },
            [&] { mPostProcessing->CommitShaderReload(); },
            [&] { mPostProcessing->DiscardShaderReload(); });
        if (mDevice->RayTracingSupported())
        {
            mPendingShaderReload.AddStage("raytracing",
                [&](std::string &stageError)
                { return mRayTracer->PrepareShaderReload(mPendingCatalog.materials, &stageError); },
                [&] { mRayTracer->CommitShaderReload(); },
                [&] { mRayTracer->DiscardShaderReload(); });
        }

        std::string prepareError;
        if (!mPendingShaderReload.PrepareAll(&prepareError))
            return fail(prepareError);

        mPendingShaderLibrary = std::move(candidateLibrary);
        mPendingSpirvAbi = std::move(candidates);
        mPendingShaderHashes = std::move(candidateHashes);
        mPendingShaderRoot = generationDirectory;
        mShaderReloadPrepared = true;
        return true;
        }
        catch (const std::exception &exception)
        {
            mPendingShaderReload.DiscardAll();
            if (error != nullptr)
                *error = std::string("shader preparation failed: ") + exception.what();
            return false;
        }
    }

    void VulkanRenderer::CommitPreparedShaderReload()
    {
        if (!mShaderReloadPrepared)
            return;
        mPendingShaderReload.CommitAll();
        mAcceptedSpirvAbi = std::move(mPendingSpirvAbi);
        mAcceptedShaderHashes = std::move(mPendingShaderHashes);
        mShaderLibrary = std::move(mPendingShaderLibrary);
        SetShaderRootOverride(std::move(mPendingShaderRoot));
        mShaderReloadPrepared = false;
        mPendingShaderRoot.clear();
        ++mShaderReloadStats.mGeneration;
        try
        {
            MIR_LOG_INFO("Shader generation {} committed ({} device idles)",
                mShaderReloadStats.mGeneration, mShaderReloadStats.mDeviceIdles);
            if (mShaderReloadListener)
                mShaderReloadListener();
        }
        catch (...)
        {
        }
    }

    void VulkanRenderer::DiscardPreparedShaderReload()
    {
        if (!mShaderReloadPrepared)
            return;
        mPendingShaderReload.DiscardAll();
        mShaderReloadPrepared = false;
        mPendingSpirvAbi.clear();
        mPendingShaderHashes.clear();
        mPendingShaderLibrary.Clear();
    }

    bool VulkanRenderer::ReloadShaders(std::string *error)
    {
        if (!PrepareShaderReload(ShaderPath(""), error))
            return false;
        CommitPreparedShaderReload();
        return true;
    }
    void VulkanRenderer::SetShaderReloadListener(std::function<void()> listener)
    {
        mShaderReloadListener = std::move(listener);
    }

    bool VulkanRenderer::RecreateSwapchain()
    {
        mDevice->WaitIdle();

        // The swapchain cannot reach ray tracing, so the facade resizes its output images.
        if (!mSwapchain->Recreate(*mWindow) ||
            (mDevice->RayTracingSupported() && !mRayTracer->Resize(mSwapchain->Extent())) ||
            !mFxComposition->Resize(mSwapchain->Extent()) ||
            !mPostProcessing->Resize(mSwapchain->Extent()))
        {
            // Recreate may have cleared the flag already; set it so the next frame retries.
            mSwapchain->MarkOutOfDate();
            return false;
        }
        mRasterSurfaces->Resize(mSwapchain->Extent());
        mSsgiTrace->Shutdown();
        if (!mRasterSurfaces->Active()) mMaterialStore->ReleaseSurfacePipelines();
        return true;
    }

    void VulkanRenderer::Shutdown()
    {
        DiscardPreparedShaderReload();
        if (mDevice->Handle() != VK_NULL_HANDLE)
        {
            ReleaseCachedMaterials();
            ReleaseCachedMeshes();
            ReleaseCachedTextures();
            mDevice->WaitIdle();

            CollectReleasedResources(UINT64_MAX);
            mFrameSync->Shutdown();

            mMeshUploads->Shutdown(*mRayTracer);

            if (mFxSystem) { mFxSystem->Shutdown(); mFxSystem = nullptr; }
            mFxComposition->Shutdown();
            mDebugDraw->Shutdown();
            mPostProcessing->Shutdown();
            mRasterSurfaces->Shutdown();
            mSsgiTrace->Shutdown();
#if MIR_WITH_RTXGI
            mDdgi->Shutdown();
#endif
            mMaterialStore->Shutdown();
            mRayTracer->Shutdown();
            if (mMaterialTable != nullptr)
                mMaterialTable->Shutdown();
            mResources->Shutdown();
            mCapture->Shutdown();
            mModelDescriptors->Shutdown();
            mCameraBuffer->Shutdown();
            mShadowMaps->Shutdown();

            mSwapchain->Shutdown();
        }

        // Last: everything above was created from the device.
        mDevice->Shutdown();
    }

    MeshHandle VulkanRenderer::CreateMesh(std::span<const Vertex> vertices, std::span<const uint32_t> indices)
    {
        // No BLAS yet: one takes about as much memory as the mesh, and a raster frame never reads it.
        return mResources->CreateMesh(vertices, indices);
    }

    bool VulkanRenderer::DestroyMesh(MeshHandle handle)
    {
        if (mResources->FindMeshSlot(handle) == nullptr)
        {
            MIR_LOG_ERROR("DestroyMesh: handle is null or stale");
            return false;
        }

        // in-flight frames (up to kFramesInFlight - 1 old ones) may still be
        // drawing from this buffer - see ReleaseSceneResources.
        mDevice->WaitIdle();

        return DestroyMeshAndBlas(handle);
    }

    bool VulkanRenderer::DestroyMeshAndBlas(MeshHandle handle)
    {
        mMeshUploads->Forget(handle);
        if (!mResources->DestroyMesh(handle))
            return false;
        mRayTracer->DestroyMeshBlas(handle.mIndex);
        return true;
    }

    bool VulkanRenderer::RequestStagingBytes(uint64_t bytes)
    {
        return mMeshUploads->RequestStagingBytes(bytes);
    }

    bool VulkanRenderer::SetUploadFrameBudget(uint64_t bytes)
    {
        return mMeshUploads->SetFrameBytes(bytes);
    }

    uint64_t VulkanRenderer::UploadFrameBudget() const
    {
        return mMeshUploads->FrameBytes();
    }

    MeshHandle VulkanRenderer::QueueMesh(MeshUploadSource source, const MeshUploadOptions &options)
    {
        return mMeshUploads->Queue(*mResources, *mRayTracer, std::move(source), options);
    }

    MeshUploadState VulkanRenderer::MeshState(MeshHandle handle) const
    {
        const vk::GpuMesh *const mesh = mResources->FindMeshSlot(handle);
        if (mesh == nullptr)
            return MeshUploadState::Stale;
        switch (mesh->mState)
        {
        case vk::GpuMeshState::Ready:
            return MeshUploadState::Ready;
        case vk::GpuMeshState::Pending:
            return MeshUploadState::Pending;
        case vk::GpuMeshState::Uploading:
            return MeshUploadState::Uploading;
        case vk::GpuMeshState::Failed:
            return MeshUploadState::Failed;
        }
        return MeshUploadState::Stale;
    }

    bool VulkanRenderer::RetireMesh(MeshHandle handle)
    {
        return mMeshUploads->Retire(*mResources, *mRayTracer, handle, mFrameSync->LastIssuedValue());
    }

    uint64_t VulkanRenderer::MeshFootprint(uint32_t vertexCount, uint32_t indexCount,
        const MeshUploadOptions &options) const
    {
        return mMeshUploads->Footprint(*mRayTracer, vertexCount, indexCount, options);
    }

    MeshStreamingStats VulkanRenderer::MeshStreaming() const
    {
        MeshStreamingStats stats;
        mMeshUploads->FillStats(stats);
        stats.textureAllocatedBytes = mResources->TextureAllocatedBytes();
        stats.deviceWaits = mDevice->DeviceWaits();
        stats.queueWaits = mDevice->QueueWaits();
        stats.skippedDraws = mSkippedDraws;
        stats.highestGeneration = mResources->HighestMeshGeneration();
        return stats;
    }

    TextureHandle VulkanRenderer::CreateTexture(const TextureDesc &desc)
    {
        return mResources->CreateTexture(desc);
    }

    TextureHandle VulkanRenderer::QueueTexture(TextureUploadSource source)
    {
        return mMeshUploads->QueueTexture(*mResources, std::move(source));
    }

    TextureUploadState VulkanRenderer::TextureState(TextureHandle handle) const
    {
        const auto *texture = mResources->FindTextureSlot(handle);
        return texture ? texture->mState : TextureUploadState::Stale;
    }

    bool VulkanRenderer::RetireTexture(TextureHandle handle)
    {
        return mMeshUploads->RetireTexture(*mResources, handle, mFrameSync->LastIssuedValue());
    }

    uint64_t VulkanRenderer::TextureFootprint(TextureHandle handle) const
    {
        const auto *texture = mResources->FindTextureSlot(handle);
        return texture ? texture->mBytes : 0;
    }

    uint64_t VulkanRenderer::TextureRetiringBytes() const
    {
        return MeshStreaming().retiringTextureBytes;
    }

    TextureHandle VulkanRenderer::ErrorTexture() const
    {
        return mResources->ErrorTexture();
    }

    MaterialHandle VulkanRenderer::CreateMaterial(const MaterialDesc &desc)
    {
        return mMaterialStore->CreateMaterial(desc, *mResources, vk::kHdrColorFormat);
    }

    bool VulkanRenderer::MaterialSlotsExhausted() const
    {
        return mMaterialStore->SlotsExhausted();
    }

    bool VulkanRenderer::UpdateMaterialRecord(MaterialHandle handle, std::span<const std::byte> bytes)
    {
        return mMaterialStore->UpdateRecord(handle, bytes);
    }

    bool VulkanRenderer::PrepareMaterialRecords(std::span<const MaterialRecordUpdate> updates, std::string *error)
    {
        return mMaterialStore->PrepareRecordBatch(updates, error);
    }

    void VulkanRenderer::CommitMaterialRecords()
    {
        mMaterialStore->CommitRecordBatch();
    }

    void VulkanRenderer::DiscardMaterialRecords()
    {
        mMaterialStore->DiscardRecordBatch();
    }

    std::optional<MaterialRecordSnapshot> VulkanRenderer::MaterialRecord(MaterialHandle handle) const
    {
        if (mMaterialStore == nullptr)
            return std::nullopt;

        MaterialRecordSnapshot snapshot;
        snapshot.mMaterial = handle;
        std::span<const std::byte> bytes;
        if (!mMaterialStore->AcceptedRecord(handle, snapshot.mLayout, bytes))
            return std::nullopt;
        snapshot.mBytes.assign(bytes.begin(), bytes.end());
        return snapshot;
    }

    void VulkanRenderer::SetMaterialRecordValidator(MaterialRecordValidator validator)
    {
        mMaterialStore->SetRecordValidator(std::move(validator));
    }

    uint32_t VulkanRenderer::MaterialRecordOffset(MaterialHandle handle) const
    {
        const vk::GpuMaterial *const material = mMaterialStore->FindMaterial(handle);
        return material != nullptr ? material->mRecordOffset : kInvalidMaterialRecordOffset;
    }

    MaterialHandle VulkanRenderer::ErrorMaterial() const
    {
        return mMaterialStore->ErrorMaterial();
    }

    uint32_t VulkanRenderer::MaterialSamplerRow(const SamplerDesc &desc)
    {
        uint32_t row = UINT32_MAX;
        if (mResources)
            mResources->AcquireSampler(desc, &row);
        return row;
    }

    const SurfaceMaterialCatalog &VulkanRenderer::SurfaceMaterials() const
    {
        return mMaterialStore->SurfaceMaterials();
    }

    bool VulkanRenderer::DestroyMaterial(MaterialHandle handle)
    {
        if (handle == mMaterialStore->DefaultMaterial() || handle == mMaterialStore->ErrorMaterial())
        {
            MIR_LOG_ERROR("DestroyMaterial: refusing to destroy a built-in material");
            return false;
        }

        if (mMaterialStore->FindMaterial(handle) == nullptr)
        {
            MIR_LOG_ERROR("DestroyMaterial: handle is null or stale");
            return false;
        }

        // in-flight frames may still be drawing with this pipeline/set
        mDevice->WaitIdle();

        return mMaterialStore->DestroyMaterial(handle);
    }

    void VulkanRenderer::ResetHistory()
    {
        mSsgiTrace->ResetHistory();
#if MIR_WITH_RTXGI
        mDdgi->ResetHistory();
#endif
        if (mPostProcessing)
            mPostProcessing->ResetHistory();
    }

    void VulkanRenderer::SetGlobalIllumination(const DdgiSettings &settings)
    {
        if (settings == mDdgiSettings) return;
        mDdgiSettings = settings;
        if (mGiWasShading) mPostProcessing->ResetHistory();
    }

    void VulkanRenderer::ReleaseSceneResources()
    {
        ReleaseCachedMaterials();
        ReleaseCachedMeshes();
        ReleaseCachedTextures();
        if (mFxSystem) mFxSystem->ClearScene();
        if (mDevice->Handle() == VK_NULL_HANDLE)
            return;

        // in-flight frames may still use them
        mDevice->WaitIdle();
        // Drain retired handles before the stores can reuse scene slots.
        CollectReleasedResources(UINT64_MAX);

        mMeshUploads->Clear(*mRayTracer);
        mRasterSurfaces->Shutdown();
        mSsgiTrace->Shutdown();
        mMaterialStore->ReleaseSurfacePipelines();
#if MIR_WITH_RTXGI
        mDdgi->Shutdown();
#endif
        mRayTracer->DestroyBlases();
        mRayTracingOutOfMemory = false;
        const std::size_t meshCount = mResources->ReleaseMeshes();
        mModelDescriptors->ResetFrames();
        // The default textures and the default and error materials stay: CreateMaterial and draws fall back to them.
        const std::size_t textureCount = mResources->ReleaseTextures();
        const std::size_t materialCount = mMaterialStore->ReleaseMaterials();

        MIR_LOG_INFO("Released {} meshes, {} materials and {} textures", meshCount, materialCount, textureCount);
    }

    void VulkanRenderer::ReleaseModelResources(std::span<const MeshHandle> meshes,
        std::span<const MaterialHandle> materials,
        std::span<const TextureHandle> textures)
    {
        if (mDevice->Handle() == VK_NULL_HANDLE)
            return;

        for (const MeshHandle handle : meshes)
            RetireMesh(handle);
        for (const TextureHandle handle : textures)
            RetireTexture(handle);
        if (!materials.empty())
            mReleasedResources.Push(mFrameSync->LastIssuedValue(), 0,
                ReleasedResources{ { materials.begin(), materials.end() } });
    }

    void VulkanRenderer::CollectReleasedResources(uint64_t completed)
    {
        mReleasedResources.Collect(completed, [this](ReleasedResources &resources)
            {
                for (const MaterialHandle handle : resources.materials)
                    mMaterialStore->DestroyMaterial(handle);
            });
    }

    bool VulkanRenderer::InitImGuiVulkanBackend()
    {
        return mImGuiOverlay->Initialize(*mDevice, *mSwapchain);
    }

    bool VulkanRenderer::SwapchainIsSrgb() const
    {
        const VkFormat format = mSwapchain->Format();
        return format == VK_FORMAT_B8G8R8A8_SRGB || format == VK_FORMAT_R8G8B8A8_SRGB ||
            format == VK_FORMAT_A8B8G8R8_SRGB_PACK32;
    }

    void VulkanRenderer::ShutdownImGuiVulkanBackend()
    {
        mImGuiOverlay->Shutdown();
    }

    void VulkanRenderer::SetImGuiDrawData(ImDrawData *drawData)
    {
        mImGuiOverlay->SetDrawData(drawData);
    }

    void VulkanRenderer::CaptureNextFrame(const std::filesystem::path &path)
    {
        mCapture->Request(path);
    }

    void VulkanRenderer::SetDirectionalLight(const glm::vec3 &direction, const glm::vec3 &color)
    {
        mCameraBuffer->SetDirectionalLight(direction, color);
    }

    void VulkanRenderer::SetAmbientColor(const glm::vec3 &color)
    {
        mCameraBuffer->SetAmbientColor(color);
    }

    void VulkanRenderer::SetPostProcessing(const PostProcessingState &state)
    {
        mCameraBuffer->SetFog(state.fog.color, state.fog.density, state.fog.start, state.fog.end);

        mPostProcessing->Apply(state, *mRayTracer, mSwapchain->Extent());
    }

    void VulkanRenderer::SetAtmosphere(const Sky &sky)
    {
        mCameraBuffer->SetAtmosphere(sky);
    }

    void VulkanRenderer::SetLights(std::span<const LightItem> lights)
    {
        mCameraBuffer->SetLights(lights);
    }

    void VulkanRenderer::SetSunShadowCaster(bool active, const glm::mat4 &viewProj, WorldPosition anchor,
        std::span<const DrawItem> items)
    {
        mShadowMaps->SetSunCaster(active, viewProj, anchor, items);
    }

    void VulkanRenderer::SetSpotShadowCaster(uint32_t slot, bool active, const glm::mat4 &viewProj,
        WorldPosition anchor, std::span<const DrawItem> items)
    {
        mShadowMaps->SetSpotCaster(slot, active, viewProj, anchor, items);
    }

    void VulkanRenderer::SetPointShadowCaster(uint32_t slot, bool active, WorldPosition position, float range,
        const std::array<PointShadowFace, 6> &faces)
    {
        mShadowMaps->SetPointCaster(slot, active, position, range, faces);
    }
}
