/**
 * @file VulkanRenderer.h
 * @author Sumin Park
 * @brief Renderer with all Vulkan signatures.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/ecs/components/WorldTransform.h>
#include <core/math/WorldCoordinates.h>
#include <core/platform/Surface.h>
#include <renderer/DebugView.h>
#include <renderer/Material.h>
#include <renderer/Mesh.h>
#include <renderer/MeshStreaming.h>
#include <renderer/PostProcessingState.h>
#include <renderer/FxSystem.h>
#include <core/Registration.h>
#include <renderer/RenderSettings.h>
#include <renderer/RtxgiConfig.h>
#include <renderer/ShaderLibrary.h>
#include <renderer/ShaderGeneration.h>
#include <renderer/ShaderReload.h>
#include <renderer/SurfaceMaterialCatalog.h>
#include <renderer/Texture.h>
#include <renderer/components/Sky.h>

#include <volk.h>

#include <glm/geometric.hpp>
#include <glm/mat3x3.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

struct ImDrawData;

namespace mir::vk
{
    class Device;
    class Swapchain;
    class FrameSync;
    struct FrameContext;
    class ResourceStore;
    class MaterialStore;
    class CameraBuffer;
    class ShadowMaps;
    class ModelDescriptors;
    class RayTracer;
    class MaterialTable;
    class RasterSurfaces;
    class SsgiTrace;
    class Ddgi;
    class PostProcessingStack;
    class FrameCapture;
    class FxCompositionPass;
    class ImGuiOverlay;
    class MeshUploader;
    class DebugDraw;
}

namespace mir
{
    struct SurfaceMaterialCatalog;

    struct RendererDesc
    {
        const ISurfaceProvider *window;
        const char *appName;
        bool enableValidation;
    };

    // One draw in the fixed Scene world, built by RenderSystem from a
    // WorldTransform + MeshRenderer pair or by ModelSystem. BuildViewDrawItems
    // (renderer/ViewSubmission.h) turns it into a DrawItem per viewport.
    struct WorldDrawItem
    {
        MeshHandle mesh;

        WorldPose pose;

        // transpose(inverse(pose.linear))
        glm::mat3 normalMatrix{ 1.0f };

        glm::vec4 tint{ 1.0f, 1.0f, 1.0f, 1.0f };

        // kNullMaterial means "use the renderer's default material".
        MaterialHandle material;

        // Morph/skin data for a model draw, read through set 2; empty for
        // everything else. Slot 2 is reserved for the material parameters the
        // renderer writes in - see vk::ModelDescriptors::Bind.
        std::span<const glm::vec4> deformation;

        // Drawn after the opaque items, back to front. BuildViewDrawItems sorts.
        bool transparent = false;

        // Power-of-two meters the camera-relative translation snaps to; 0 = none.
        float translationGrid = 0.0f;

        // Last frame's pose, for motion blur's velocity buffer. nullopt means
        // "no history" (new this frame, or the producer doesn't track it) -
        // BuildViewDrawItems then falls back to `pose` itself, i.e. zero velocity.
        std::optional<WorldPose> previousPose;
    };

    // One draw call's worth of data for one viewport. Temporary: it lives only
    // for the frame's submission and is never written back to the Scene.
    struct DrawItem
    {
        MeshHandle mesh;

        // Relative to this viewport's camera: the translation is
        // pose.position - camera, subtracted in world precision before narrowing.
        glm::mat4 model{ 1.0f };

        glm::mat3 normalMatrix{ 1.0f };

        glm::vec4 tint{ 1.0f, 1.0f, 1.0f, 1.0f };

        MaterialHandle material;

        std::span<const glm::vec4> deformation;

        bool transparent = false;

        // Last frame's pose narrowed the same way as `model`, but relative to
        // THIS frame's camera (not last frame's) - see BuildViewDrawItems.
        // Equal to `model` when there is no history, so a still object under a
        // still camera comes out to exactly zero velocity.
        glm::mat4 previousModel{ 1.0f };
    };

    // Draw items produced outside the MeshRenderer query (by ModelSystem),
    // picked up by RenderSystem as a World resource.
    struct ModelDrawItems { std::vector<WorldDrawItem> items; };

    // A Point/Spot/Area light in world space; the renderer narrows it against
    // each view's camera, as it does draw items.
    struct LightItem
    {
        WorldPosition position;
        float range = 0.0f;
        glm::vec3 direction{ 0.0f, -1.0f, 0.0f };
        float spotCos = -1.0f; // cos(half-angle); -1 = no cone (Point/Area)
        glm::vec3 color{ 0.0f }; // color * intensity
        int32_t shadowSlot = -1; // -1 = none, -2 = Area (never shadowed)
    };

    struct PointShadowFace
    {
        glm::mat4 viewProj{ 1.0f };
        std::span<const DrawItem> items;
    };

    struct RenderView
    {
        VkRect2D rect{};
        std::span<const DrawItem> items;
        glm::mat4 viewProj{ 1.0f };
        WorldPosition cameraPosition;

        float nearZ = 0.1f;
        // farZ 0 = no far plane
        float farZ = 0.0f;
        // Optional extended GI scene, used while DDGI can update this camera's volume.
        // Coordinates are relative to this view's camera.
        // nullopt keeps the ordinary RT path; an empty span is an empty scene.
        std::optional<std::span<const DrawItem>> rayTracingItems;
        // False when narrowing the extended scene omitted world draws. The
        // final camera trace can still run; GI must fall back for this frame.
        bool rayTracingSceneComplete = true;
        // Stable camera identity for temporal effects; callers with no ID reset history on camera switches.
        uint64_t historyId = 0;
        // The camera jumped, this view keeps no history
        bool cut = false;
        // Editor-only debug drawing for this view; the default is the final render alone.
        DebugViewSettings debugView;
        // Vertex pairs drawn as lines over everything, never hidden by the scene; colliders while debugView asks.
        std::span<const DebugLinePoint> debugLines;
        // The lens FOV that produced viewProj, for rotation-invariant ray-cone math.
        float fovYDegrees = 0.0f;
    };

    class VulkanRenderer
    {
    public:
        VulkanRenderer();
        ~VulkanRenderer();

        bool Initialize(const RendererDesc &desc);

        bool InitImGuiVulkanBackend();
        void ShutdownImGuiVulkanBackend();
        bool SwapchainIsSrgb() const;

        // abstraction from imgui
        void SetImGuiDrawData(ImDrawData *drawData);
        MeshHandle CreateMesh(std::span<const Vertex> vertices, std::span<const uint32_t> indices);

        bool DestroyMesh(MeshHandle handle);

        TextureHandle CreateTexture(const TextureDesc &desc);

        TextureHandle ErrorTexture() const;

        MaterialHandle CreateMaterial(const MaterialDesc &desc);

        bool DestroyMaterial(MaterialHandle handle);

        // Raw reflected bytes for one material's record, published at the next
        // BeginFrame. False without shared material tables, or for a stale handle.
        bool UpdateMaterialRecord(MaterialHandle handle, std::span<const std::byte> bytes);
        uint32_t MaterialRecordOffset(MaterialHandle handle) const;

        MaterialHandle ErrorMaterial() const;

        // Stable shared-table rows for a material record's TextureRef fields.
        uint32_t MaterialSamplerRow(const SamplerDesc &desc);
        const SurfaceMaterialCatalog &SurfaceMaterials() const;
        struct ShaderReloadStats
        {
            uint32_t mDeviceIdles = 0;
            uint64_t mGeneration = 0;
        };

        // Development-only hot reload of every registered shader call site.
        // Files are already published beside the exe; this validates the whole
        // batch, builds all replacement pipelines off to the side and swaps them
        // after one device idle. Failure leaves the previous generation live.
        bool ReloadShaders(std::string *error = nullptr);
        // Publication protocol: read the candidate generation, validate and
        // build every GPU candidate off to the side, then let the caller promote
        // the generation pointer before the no-fail commit.
        bool PrepareShaderReload(const std::filesystem::path &generationDirectory,
            std::string *error = nullptr);
        void CommitPreparedShaderReload();
        void DiscardPreparedShaderReload();
        bool ShaderReloadPrepared() const { return mShaderReloadPrepared; }
        bool ShaderReloadAvailable() const { return mShaderReloadReady; }
        const ShaderReloadStats &ReloadStats() const { return mShaderReloadStats; }
        // Runs after a successful publication, whether or not a panel is open.
        void SetShaderReloadListener(std::function<void()> listener);

        void CaptureNextFrame(const std::filesystem::path &path);

        void ReleaseSceneResources();

        // the next frame starts temporal accumulation fresh (DLSS ray reconstruction's history)
        void ResetHistory();

        // Exactly one FX provider, unregistration completes recorded uses before releasing it.
        Registration RegisterFxSystem(IFxSystem &system);

        // Releases a selected model generation after a replacement has been
        // prepared. Handles not present in these spans remain valid.
        void ReleaseModelResources(std::span<const MeshHandle> meshes,
            std::span<const MaterialHandle> materials,
            std::span<const TextureHandle> textures);

        // Asynchronous meshes (renderer/MeshStreaming.h), none of which waits on the GPU: a queued mesh is copied
        // through the persistent staging ring at the start of a frame, within the per-frame byte budget, and is
        // Ready once that frame completes; a retired one is freed once every frame that may use it completes.
        // CreateMesh and DestroyMesh stay synchronous.
        //
        // Grows the staging ring to at least this size, true if it is that big afterwards.
        bool RequestStagingBytes(uint64_t bytes);
        // The GPU bytes one frame may write, shared by every consumer.
        bool SetUploadFrameBudget(uint64_t bytes);
        uint64_t UploadFrameBudget() const;
        MeshHandle QueueMesh(std::vector<Vertex> vertices, std::vector<uint32_t> indices,
            const MeshUploadOptions &options);
        MeshUploadState MeshState(MeshHandle handle) const;
        // The handle goes stale at once; false when it already was.
        bool RetireMesh(MeshHandle handle);
        // GPU bytes a queued mesh of this size holds, BLAS included.
        uint64_t MeshFootprint(uint32_t vertexCount, uint32_t indexCount, const MeshUploadOptions &options) const;
        MeshStreamingStats MeshStreaming() const;

        VkExtent2D SwapchainExtent() const;

        void RequestSwapchainRecreate();

        void DrawFrame(std::span<const RenderView> views);

        void TraceFrame(std::span<const RenderView> views);

        bool RayTracingSupported() const;

        // Whether a frame with Ray Tracing requested actually goes down the TraceFrame path -
        // the one flag AA mode clamping and the panel must agree with dispatch on.
        bool RayTracingActive(bool requested) const;

        // false: the GPU cannot draw lines, and the Wireframe view shows the shaded scene
        bool WireframeAvailable() const;

        // NGX support and a Ray Reconstruction feature that hasn't failed. Goes false for good
        // after an init/resize/evaluate failure, so Dlss stops being offered or selected.
        bool DlssAvailable() const;

        struct GpuMemoryUsage
        {
            uint64_t mUsageBytes = 0;
            uint64_t mBudgetBytes = 0;
        };

        GpuMemoryUsage GetGpuMemoryUsage() const;
        uint64_t FxCompositionMemoryBytes() const;

        constexpr static uint32_t kMaxSpotShadows = 4;
        constexpr static uint32_t kMaxPointShadows = 2;

        constexpr static uint32_t kSunShadowResolution = 2048;
        constexpr static float kSunShadowHalfExtent = 25.0f;
        constexpr static float kSunShadowNearZ = 0.5f;

        constexpr static float kSpotShadowNearZ = 0.05f;
        constexpr static float kPointShadowNearZ = 0.05f;

        void SetClearColor(const glm::vec4 &color) { mClearColor = color; }

        void SetDirectionalLight(const glm::vec3 &direction, const glm::vec3 &color);

        void SetAmbientColor(const glm::vec3 &color);

        // Every Post-Processing effect for the frame, already resolved by ResolvePostProcessing.
        // A disabled Bloom/DepthOfField/MotionBlur skips its GPU chain for the frame.
        void SetPostProcessing(const PostProcessingState &state);
        void SetGlobalIllumination(const DdgiSettings &settings);
        void SetScreenSpaceGlobalIllumination(const SsgiSettings &settings) { mSsgiSettings = settings; }

        void SetAtmosphere(const Sky &sky);

        void SetSkyVisible(bool visible) { mSkyVisible = visible; }

        void SetLights(std::span<const LightItem> lights);

        void SetSunShadowCaster(bool active, const glm::mat4 &viewProj, WorldPosition anchor,
            std::span<const DrawItem> items);

        void SetSpotShadowCaster(uint32_t slot, bool active, const glm::mat4 &viewProj, WorldPosition anchor,
            std::span<const DrawItem> items);

        void SetPointShadowCaster(uint32_t slot, bool active, WorldPosition position, float range,
            const std::array<PointShadowFace, 6> &faces);

        void Shutdown();

    private:
        FxDeviceContext FxDevice() const;
        bool RecreateSwapchain();
        // Shared prelude of DrawFrame and TraceFrame; nullopt skips the frame.
        std::optional<vk::FrameContext> BeginFrame();
        // No wait: the caller idles the device first. False for a null or stale handle.
        bool DestroyMeshAndBlas(MeshHandle handle);

        void RecordRtFrame(VkCommandBuffer cmd, uint32_t imageIndex, std::span<const RenderView> views);

        void RecordCommands(VkCommandBuffer cmd, uint32_t imageIndex, std::span<const RenderView> views);
        // Called inside the composite pass, after the composite and before ImGui: when a view has a
        // debug view on, ends that pass, draws the wireframe and debug lines, and begins the ImGui pass
        // again. sceneDepth is the raster path's depth image, null on the ray-traced path.
        void RecordDebugViews(VkCommandBuffer cmd, uint32_t imageIndex, std::span<const RenderView> views,
            VkImage sceneDepth, VkImageView sceneDepthView);
        enum class RasterPass { Scene, Opaque, Surface, Transparent };
        uint32_t RecordView(VkCommandBuffer cmd, const RenderView &view, uint32_t viewIndex,
            RasterPass pass = RasterPass::Scene, uint32_t cameraOffset = 0);
        uint32_t WriteCameraData(const RenderView &view, uint32_t viewIndex, bool applyJitter = false,
            glm::vec2 *outJitter = nullptr);

        void RegisterShaderConsumers(ShaderLibrary &library, const SurfaceMaterialCatalog *catalog);
        bool LoadAcceptedShaderSignatures(std::string *error);
        void InitializeShaderLibrary();

    private:
        glm::vec4 mClearColor{ 0.02f, 0.02f, 0.02f, 1.0f };

        bool mSkyVisible = false;

        const ISurfaceProvider *mWindow;

        // Created in the constructor, Initialize/Shutdown bound its Vulkan objects.
        std::unique_ptr<vk::Device> mDevice;
        std::unique_ptr<vk::Swapchain> mSwapchain;
        std::unique_ptr<vk::FrameSync> mFrameSync;
        std::unique_ptr<vk::ResourceStore> mResources;
        // Only on a device with descriptor indexing; null keeps the legacy material path.
        std::unique_ptr<vk::MaterialTable> mMaterialTable;
        std::unique_ptr<vk::MaterialStore> mMaterialStore;
        std::unique_ptr<vk::RasterSurfaces> mRasterSurfaces;
        std::unique_ptr<vk::SsgiTrace> mSsgiTrace;
        SsgiSettings mSsgiSettings;

        std::unique_ptr<vk::CameraBuffer> mCameraBuffer;
        std::unique_ptr<vk::ShadowMaps> mShadowMaps;
        std::unique_ptr<vk::ModelDescriptors> mModelDescriptors;
        // Initialized only on a device with ray tracing; its Shutdown is a no-op otherwise.
        std::unique_ptr<vk::RayTracer> mRayTracer;
#if MIR_WITH_RTXGI
        std::unique_ptr<vk::Ddgi> mDdgi;
#endif
        DdgiSettings mDdgiSettings;
        bool mGiWasShading = false;
        // views[0]'s camera on the last TraceFrame.
        uint64_t mTracedHistoryId = 0;

        // Every screen-space effect between the scene/trace pass and the swapchain.
        std::unique_ptr<vk::PostProcessingStack> mPostProcessing;
        std::unique_ptr<vk::FxCompositionPass> mFxComposition;
        IFxSystem *mFxSystem = nullptr;
        std::shared_ptr<const void> mFxLifetime = std::make_shared<int>(0);

        std::unique_ptr<vk::FrameCapture> mCapture;

        std::unique_ptr<vk::ImGuiOverlay> mImGuiOverlay;

        std::unique_ptr<vk::DebugDraw> mDebugDraw;

        std::unique_ptr<vk::MeshUploader> mMeshUploads;

        uint64_t mSkippedDraws = 0;

        ShaderLibrary mShaderLibrary;
        std::map<std::string, ShaderSpirvAbi> mAcceptedSpirvAbi;
        std::map<std::string, uint64_t> mAcceptedShaderHashes, mPendingShaderHashes;
        ShaderReloadTransaction mPendingShaderReload;
        ShaderLibrary mPendingShaderLibrary;
        std::map<std::string, ShaderSpirvAbi> mPendingSpirvAbi;
        std::filesystem::path mPendingShaderRoot;
        SurfaceMaterialCatalog mPendingCatalog;
        bool mShaderReloadPrepared = false;
        ShaderReloadStats mShaderReloadStats;
        std::function<void()> mShaderReloadListener;
        bool mShaderReloadReady = false;
    };
}
