/**
 * @file RenderSystem.h
 * @author Sumin Park
 * @brief Drives the renderer from the ECS, in SystemPhase::Render.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once
#include "renderer/ActiveCamera.h"
#include "renderer/CameraMath.h"
#include "renderer/ViewDrawSource.h"
#include "renderer/ViewSubmission.h"
#include "renderer/VulkanRenderer.h"

#include <core/ecs/Query.h>
#include <core/ecs/System.h>
#include <core/ecs/components/Name.h>
#include <core/ecs/components/WorldTransform.h>
#include <core/log/Log.h>
#include <renderer/PostProcessingSettings.h>
#include <renderer/PostProcessingState.h>
#include <renderer/RenderSettings.h>
#include <renderer/RtxgiConfig.h>
#include <renderer/components/Camera.h>
#include <renderer/components/Light.h>
#include <renderer/components/MeshRenderer.h>
#include <renderer/components/Sky.h>

#include <glm/gtc/matrix_inverse.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mir
{
    using ViewportId = size_t;

    constexpr glm::vec3 kDefaultLightColor{ 0.85f, 0.85f, 0.85f };

    // What a source World gets when it authors neither a Sun nor a Sky.
    struct SourceLighting
    {
        bool mUseDefaultSun = false;
        bool mUseDefaultSky = false;
    };

    inline SourceLighting ResolveSourceLighting(bool haveSun, bool haveSky, bool sourceOverride)
    {
        if (!sourceOverride || haveSun || haveSky)
            return {};
        return SourceLighting{ true, true };
    }

    struct Viewport
    {
        // zero = the whole swapchain
        VkRect2D mRect{};

        Entity mCameraEntity = kNullEntity;

        WorldPose mExternalCameraPose;
        Camera mExternalCamera{};
        bool mUseExternalCamera = false;

        DebugViewSettings mDebugView;
    };

    class RenderSystem final : public ISystem
    {
    public:
        explicit RenderSystem(VulkanRenderer &renderer)
            : mRenderer(renderer)
        {
            // Viewport 0 : whole window, the active World Camera.
            mViewports.push_back(Viewport{});
        }

        void OnStart(SystemContext &context) override { BindQueries(context.world); }

        // Draws @p world's entities, lights and view sources instead of the World this
        // system runs in; RenderSettings and post-processing stay the main World's.
        // Null draws the main World again.
        void SetSourceWorld(World *world)
        {
            if (world == mSourceWorld)
                return;
            mSourceWorld = world;
            mPreviousPoses.clear();
            mCurrentPoses.clear();
            mRenderer.ResetHistory();
        }

        ViewportId AddViewport(VkRect2D rect)
        {
            mViewports.push_back(Viewport{ .mRect = rect });
            return mViewports.size() - 1;
        }

        void RemoveViewport(ViewportId id)
        {
            if (id != 0 && id < mViewports.size())
                mViewports.erase(mViewports.begin() + static_cast<ptrdiff_t>(id));
        }

        void SetViewportRect(ViewportId id, VkRect2D rect)
        {
            if (id < mViewports.size())
                mViewports[id].mRect = rect;
        }

        // Points this viewport at a Camera entity in the World (game cameras).
        void SetViewportCamera(ViewportId id, Entity camera)
        {
            if (id < mViewports.size())
            {
                mViewports[id].mCameraEntity = camera;
                mViewports[id].mUseExternalCamera = false;
            }
        }

        void SetExternalCamera(ViewportId id, const WorldPose &cameraPose, const Camera &camera)
        {
            if (id < mViewports.size())
            {
                mViewports[id].mExternalCameraPose = cameraPose;
                mViewports[id].mExternalCamera = camera;
                mViewports[id].mUseExternalCamera = true;
            }
        }

        // The editor sets this on its scene viewport; a game build never does.
        void SetViewportDebugView(ViewportId id, const DebugViewSettings &debugView)
        {
            if (id < mViewports.size())
                mViewports[id].mDebugView = debugView;
        }

        // Each scene load and Play session reports a missing camera once.
        void ResetCameraReports()
        {
            mWarnedNoCamera = false;
            mWarnedNoActiveCamera = false;
        }

        void OnUpdate(SystemContext &context) override
        {
            const RenderSettings &renderSettings = context.world.Resource<RenderSettings>();
            mRenderer.SetGlobalIllumination(renderSettings.ddgi);
            mRenderer.SetScreenSpaceGlobalIllumination(renderSettings.ssgi);

            World &source = mSourceWorld != nullptr ? *mSourceWorld : context.world;
            BindQueries(source);

            bool haveSunLight = false;
            glm::vec3 sunDirWS{ 0.0f, 1.0f, 0.0f };
            mLightItems.clear();
            mLightQuery->ForEach([&](Entity, const WorldTransform &world, const Light &light)
                {
                    if (light.mType == LightType::Sun)
                    {
                        if (haveSunLight)
                            return;
                        sunDirWS = world.Linear() * kSunReferenceLocalDirection;
                        mRenderer.SetDirectionalLight(sunDirWS, light.mColor * light.mIntensity);
                        haveSunLight = true;
                        return;
                    }

                    LightItem item{};
                    item.position = world.Position();
                    item.range = light.mRange;
                    item.direction = glm::normalize(world.Linear() * glm::vec3(0.0f, 0.0f, -1.0f));
                    item.spotCos = light.mType == LightType::Spot
                        ? std::cos(glm::radians(light.mSpotAngleDegrees * 0.5f))
                        : -1.0f;
                    item.color = light.mColor * light.mIntensity;
                    item.shadowSlot = light.mType == LightType::Area ? -2 : -1;
                    mLightItems.push_back(item);
                });

            bool haveSky = false;
            mSkyQuery->ForEach([&](Entity, const WorldTransform &world, const Sky &sky)
                {
                    if (haveSky)
                        return;
                    if (!haveSunLight)
                    {
                        sunDirWS = world.Linear() * kSunReferenceLocalDirection;
                        mRenderer.SetDirectionalLight(sunDirWS, kDefaultLightColor);
                    }
                    mRenderer.SetAtmosphere(sky);
                    haveSky = true;
                });

            const SourceLighting sourceLighting = ResolveSourceLighting(haveSunLight, haveSky, mSourceWorld != nullptr);
            if (sourceLighting.mUseDefaultSun)
            {
                sunDirWS = kSunReferenceLocalDirection;
                mRenderer.SetDirectionalLight(sunDirWS, kDefaultLightColor);
            }
            if (sourceLighting.mUseDefaultSky)
            {
                mRenderer.SetAtmosphere(Sky{});
                haveSky = true;
            }
            mRenderer.SetSkyVisible(haveSky);

            // No Camera at all draws from the origin with a default lens.
            const PlayCamera playCamera = ResolvePlayCamera(source);
            WorldPose defaultCameraPose;
            Camera defaultCamera{};
            if (!playCamera.mEntity.IsNull())
            {
                defaultCameraPose = source.GetComponent<WorldTransform>(playCamera.mEntity)->Pose();
                defaultCamera = *source.GetComponent<Camera>(playCamera.mEntity);
            }
            bool usedDefaultCamera = false;

            const VkExtent2D swapchainExtent = mRenderer.SwapchainExtent();

            // Every viewport's camera, resolved once; view draw sources and the views share it.
            mViewCameras.clear();
            bool warnNoCamera = false;
            for (size_t i = 0; i < mViewports.size(); ++i)
            {
                const Viewport &viewport = mViewports[i];

                WorldPose cameraPose;
                Camera camera{};
                bool haveCamera = false;

                if (viewport.mUseExternalCamera)
                {
                    cameraPose = viewport.mExternalCameraPose;
                    camera = viewport.mExternalCamera;
                    haveCamera = true;
                }
                else if (viewport.mCameraEntity != kNullEntity)
                {
                    mCameraQuery->ForEach([&](Entity e, const WorldTransform &world, const Camera &cam)
                        {
                            if (e != viewport.mCameraEntity)
                                return;
                            cameraPose = world.Pose();
                            camera = cam;
                            haveCamera = true;
                        });
                }
                else
                {
                    cameraPose = defaultCameraPose;
                    camera = defaultCamera;
                    haveCamera = true;
                    usedDefaultCamera = true;
                }

                if (!haveCamera)
                {
                    warnNoCamera = true;
                    continue;
                }

                const VkExtent2D viewExtent = (viewport.mRect.extent.width > 0 && viewport.mRect.extent.height > 0)
                    ? viewport.mRect.extent
                    : swapchainExtent;
                const float aspect = viewExtent.height == 0
                    ? 1.0f
                    : static_cast<float>(viewExtent.width) / static_cast<float>(viewExtent.height);
                mViewCameras.push_back(ViewCamera{ i, cameraPose, camera, viewExtent,
                                                  MakeCameraRelativeViewProjection(cameraPose, camera, aspect) });
            }

            // Post effects read viewport 0's lens: the editor camera's in Edit, the active Camera's in Play.
            const bool havePrimaryView = !mViewCameras.empty() && mViewCameras.front().viewport == 0;
            mRenderer.SetPostProcessing(ResolvePostProcessing(context.world.Resource<PostProcessingSettings>(),
                havePrimaryView ? mViewCameras.front().lens : Camera{},
                mRenderer.RayTracingActive(renderSettings.rayTracing), mRenderer.DlssAvailable()));

            ViewDrawSources *viewSources = source.TryResource<ViewDrawSources>();
            if (viewSources)
                for (IViewDrawSource *source : viewSources->sources)
                    source->Prepare(mViewCameras);

            // Shadow maps follow the primary rendered viewport, including the
            // editor's external camera and explicitly selected game cameras.
            const bool haveShadowCamera = !mViewCameras.empty() && mViewCameras.front().viewport == 0;
            const WorldPose shadowCameraPose = haveShadowCamera ? mViewCameras.front().pose : WorldPose{};

            // The world draws plus what the sources draw inside that volume at the primary view's detail.
            const auto shadowCasters = [&](WorldPosition origin,
                const glm::mat4 &viewProj) -> std::span<const WorldDrawItem>
                {
                    if (!viewSources || viewSources->sources.empty() || !haveShadowCamera)
                        return mDrawItems;
                    mPassItems.assign(mDrawItems.begin(), mDrawItems.end());
                    for (IViewDrawSource *source : viewSources->sources)
                        source->AppendVolumeDraws(0, origin, viewProj, mPassItems);
                    return mPassItems;
                };

            if (haveShadowCamera)
            {
                const auto assignNearest = [&](bool wantSpot, uint32_t cap)
                    {
                        std::vector<std::pair<float, LightItem *>> candidates;
                        for (LightItem &item : mLightItems)
                        {
                            const bool isSpot = item.spotCos != -1.0f;
                            if (isSpot != wantSpot || item.shadowSlot == -2)
                                continue;
                            const std::optional<glm::vec3> relative =
                                TryCameraRelativeFloat(item.position, shadowCameraPose.position);
                            candidates.emplace_back(
                                relative ? glm::length(*relative) : std::numeric_limits<float>::max(), &item);
                        }
                        std::sort(candidates.begin(), candidates.end(),
                            [](const auto &a, const auto &b) { return a.first < b.first; });
                        for (uint32_t slot = 0; slot < cap && slot < candidates.size(); ++slot)
                            candidates[slot].second->shadowSlot = static_cast<int32_t>(slot);
                    };
                assignNearest(/*wantSpot*/ true, VulkanRenderer::kMaxSpotShadows);
                assignNearest(/*wantSpot*/ false, VulkanRenderer::kMaxPointShadows);
            }
            mRenderer.SetLights(mLightItems);

            mDrawItems.clear();
            mCurrentPoses.clear();
            mMeshQuery->ForEach([this](Entity entity, const WorldTransform &world, const MeshRenderer &renderer)
                {
                    const glm::mat3 normalMatrix = glm::inverseTranspose(world.Linear());
                    WorldDrawItem item{
                        renderer.mesh, world.Pose(), normalMatrix, renderer.tint, renderer.material };

                    const uint64_t key = PackEntity(entity);
                    const auto previous = mPreviousPoses.find(key);
                    if (previous != mPreviousPoses.end())
                        item.previousPose = previous->second;
                    mCurrentPoses[key] = world.Pose();

                    mDrawItems.push_back(item);
                });
            std::swap(mPreviousPoses, mCurrentPoses);

            // ModelSystem builds its own items (meshes that came from a model
            // asset, with their morph/skin data attached).
            if (const ModelDrawItems *models = source.TryResource<ModelDrawItems>())
                mDrawItems.insert(mDrawItems.end(), models->items.begin(), models->items.end());

            const bool haveDirectionalLight = haveSunLight || haveSky;
            mSunShadowItems.clear();
            if (haveDirectionalLight && haveShadowCamera)
            {
                const glm::mat4 sunShadowViewProj = MakeOrthographicViewProjection(
                    -sunDirWS, VulkanRenderer::kSunShadowHalfExtent, VulkanRenderer::kSunShadowNearZ);
                const WorldPosition sunAnchor = SnapToShadowTexels(shadowCameraPose.position, -sunDirWS,
                    2.0f * VulkanRenderer::kSunShadowHalfExtent / VulkanRenderer::kSunShadowResolution);
                BuildViewDrawItems(shadowCasters(sunAnchor, sunShadowViewProj),
                    sunAnchor, sunShadowViewProj, mSunShadowItems);
                mRenderer.SetSunShadowCaster(true, sunShadowViewProj, sunAnchor, mSunShadowItems);
            }
            else
            {
                mRenderer.SetSunShadowCaster(false, glm::mat4{ 1.0f }, WorldPosition{}, mSunShadowItems);
            }

            {
                constexpr std::array<glm::vec3, 6> kPointFaceForward{ {
                    { 1, 0, 0 },{ -1, 0, 0 },{ 0, 1, 0 },{ 0, -1, 0 },{ 0, 0, 1 },{ 0, 0, -1 } } };
                constexpr std::array<glm::vec3, 6> kPointFaceUp{ {
                    { 0, -1, 0 },{ 0, -1, 0 },{ 0, 0, 1 },{ 0, 0, -1 },{ 0, -1, 0 },{ 0, -1, 0 } } };

                std::array<bool, VulkanRenderer::kMaxSpotShadows> spotSlotFilled{};
                std::array<bool, VulkanRenderer::kMaxPointShadows> pointSlotFilled{};

                for (LightItem &item : mLightItems)
                {
                    if (item.shadowSlot < 0)
                        continue;
                    const uint32_t slot = static_cast<uint32_t>(item.shadowSlot);

                    if (item.spotCos != -1.0f) // Spot
                    {
                        if (slot >= VulkanRenderer::kMaxSpotShadows)
                            continue;
                        const float fovYDegrees = 2.0f * glm::degrees(std::acos(item.spotCos));
                        const glm::vec3 upHint =
                            std::abs(item.direction.y) > 0.99f ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
                        const glm::mat4 spotViewProj = MakeForwardZPerspectiveViewProjection(
                            item.direction, upHint, fovYDegrees, VulkanRenderer::kSpotShadowNearZ, item.range);

                        mSpotShadowItems[slot].clear();
                        BuildViewDrawItems(shadowCasters(item.position, spotViewProj), item.position, spotViewProj,
                            mSpotShadowItems[slot]);
                        mRenderer.SetSpotShadowCaster(slot, true, spotViewProj, item.position, mSpotShadowItems[slot]);
                        spotSlotFilled[slot] = true;
                    }
                    else // Point (Area never reaches here - it's never assigned a shadowSlot)
                    {
                        if (slot >= VulkanRenderer::kMaxPointShadows)
                            continue;
                        std::array<PointShadowFace, 6> faces{};
                        for (int face = 0; face < 6; ++face)
                        {
                            const glm::mat4 faceViewProj = MakeForwardZPerspectiveViewProjection(
                                kPointFaceForward[face], kPointFaceUp[face], 90.0f,
                                VulkanRenderer::kPointShadowNearZ, item.range);

                            mPointShadowItems[slot][face].clear();
                            BuildViewDrawItems(shadowCasters(item.position, faceViewProj), item.position, faceViewProj,
                                mPointShadowItems[slot][face]);
                            faces[face] = PointShadowFace{ faceViewProj, mPointShadowItems[slot][face] };
                        }
                        mRenderer.SetPointShadowCaster(slot, true, item.position, item.range, faces);
                        pointSlotFilled[slot] = true;
                    }
                }

                for (uint32_t slot = 0; slot < VulkanRenderer::kMaxSpotShadows; ++slot)
                    if (!spotSlotFilled[slot])
                        mRenderer.SetSpotShadowCaster(slot, false, glm::mat4{ 1.0f }, WorldPosition{}, {});
                for (uint32_t slot = 0; slot < VulkanRenderer::kMaxPointShadows; ++slot)
                    if (!pointSlotFilled[slot])
                        mRenderer.SetPointShadowCaster(
                            slot, false, WorldPosition{}, 0.0f, std::array<PointShadowFace, 6>{});
            }

            mDrawItemsPerViewport.resize(mViewCameras.size());
            mRenderViews.clear();

            size_t skippedItems = 0;

            for (size_t v = 0; v < mViewCameras.size(); ++v)
            {
                const ViewCamera &view = mViewCameras[v];
                std::span<const WorldDrawItem> worldItems = mDrawItems;
                if (viewSources && !viewSources->sources.empty())
                {
                    mPassItems.assign(mDrawItems.begin(), mDrawItems.end());
                    for (IViewDrawSource *source : viewSources->sources)
                        source->AppendViewDraws(v, mPassItems);
                    worldItems = mPassItems;
                }

                // Each viewport narrows against its own camera, so the same
                // world item gets a different model matrix per view.
                std::vector<DrawItem> &items = mDrawItemsPerViewport[v];
                skippedItems += BuildViewDrawItems(worldItems, view.pose.position, view.viewProj, items);

                mRenderViews.push_back(RenderView{ mViewports[view.viewport].mRect, items, view.viewProj,
                                                  view.pose.position, view.lens.mNearZ,
                                                  view.lens.HasFarPlane() ? view.lens.mFarZ : 0.0f });
                const auto &viewport = mViewports[view.viewport];
                mRenderViews.back().historyId = viewport.mUseExternalCamera ? UINT64_MAX :
                    PackEntity(viewport.mCameraEntity.IsNull() ? playCamera.mEntity : viewport.mCameraEntity);
                mRenderViews.back().debugView = viewport.mDebugView;
            }

            if (warnNoCamera && !mWarnedNoCamera)
            {
                MIR_LOG_WARN("RenderSystem: a viewport's Camera entity is not in the world; nothing will be drawn for it");
                mWarnedNoCamera = true;
            }

            if (usedDefaultCamera && !playCamera.mActive && !mWarnedNoActiveCamera)
            {
                if (playCamera.mEntity.IsNull())
                {
                    MIR_LOG_WARN("RenderSystem: the scene has no Camera; drawing from the origin with a default lens");
                }
                else
                {
                    const Name *name = source.GetComponent<Name>(playCamera.mEntity);
                    const std::string label = name != nullptr ? std::string(name->mValue)
                                                              : std::format("entity {}", playCamera.mEntity.mIndex);
                    MIR_LOG_WARN("RenderSystem: no Camera is active; drawing through '{}' until one is", label);
                }
                mWarnedNoActiveCamera = true;
            }

            if (skippedItems > 0 && !mWarnedSkippedItems)
            {
                MIR_LOG_WARN("RenderSystem: {} draw(s) are too far from a viewport camera for float and were not drawn",
                    skippedItems);
                mWarnedSkippedItems = true;
            }

            // Wireframe shows no shading, so it never pays for a trace
            const bool wireframeOnly = mRenderViews.size() == 1 && mRenderer.WireframeAvailable() &&
                mRenderViews.front().debugView.shading == ViewportShading::Wireframe;
            // leaving or entering it swaps paths; temporal effects must not blend across the switch
            if (wireframeOnly != mWasWireframeOnly)
            {
                mRenderer.ResetHistory();
                mWasWireframeOnly = wireframeOnly;
            }

            const bool traceable = mRenderer.RayTracingActive(renderSettings.rayTracing);
            if (traceable && mRenderViews.size() == 1 && !wireframeOnly)
            {
                mRayTracingItems.clear();
                if (kRtxgiCompiledIn && renderSettings.ddgi.enabled)
                    if (MakeGiTraceVolume(renderSettings.ddgi))
                    {
                        const std::span<IViewDrawSource *const> sources = viewSources
                            ? std::span<IViewDrawSource *const>(viewSources->sources)
                            : std::span<IViewDrawSource *const>{};
                        CollectRayTracingWorldDraws(mDrawItems, sources, 0, mRayTracingWorldItems);
                        // Identity clip matrix preserves insertion order within opaque/
                        // transparent groups; no camera-depth sorting for the TLAS.
                        mRenderViews.front().rayTracingSceneComplete =
                            BuildViewDrawItems(mRayTracingWorldItems, mRenderViews.front().cameraPosition,
                                glm::mat4(1.0f), mRayTracingItems) == 0;
                        mRenderViews.front().rayTracingItems = mRayTracingItems;
                    }
                mRenderer.TraceFrame(mRenderViews);
            }
            else
            {
                if (renderSettings.rayTracing && !traceable && !mWarnedNoRayTracing)
                {
                    MIR_LOG_WARN("RenderSystem: Ray Tracing is on in Project Settings but this device/driver "
                        "doesn't support it; falling back to raster");
                    mWarnedNoRayTracing = true;
                }
                if (traceable && mRenderViews.size() > 1 && !mWarnedMultiViewRayTracing)
                {
                    MIR_LOG_WARN("RenderSystem: Ray Tracing draws a single viewport; {} viewports are "
                        "falling back to raster", mRenderViews.size());
                    mWarnedMultiViewRayTracing = true;
                }
                mRenderer.DrawFrame(mRenderViews);
            }
        }

    private:
        void BindQueries(World &world)
        {
            mMeshQuery = &world.GetOrCreateQuery<const WorldTransform, const MeshRenderer>();
            mCameraQuery = &world.GetOrCreateQuery<const WorldTransform, const Camera>();
            mSkyQuery = &world.GetOrCreateQuery<const WorldTransform, const Sky>();
            mLightQuery = &world.GetOrCreateQuery<const WorldTransform, const Light>();
        }

        VulkanRenderer &mRenderer;
        World *mSourceWorld = nullptr;
        Query<const WorldTransform, const MeshRenderer> *mMeshQuery = nullptr;
        Query<const WorldTransform, const Camera> *mCameraQuery = nullptr;
        Query<const WorldTransform, const Sky> *mSkyQuery = nullptr;
        Query<const WorldTransform, const Light> *mLightQuery = nullptr;

        std::vector<Viewport> mViewports;

        std::vector<LightItem> mLightItems;
        std::vector<WorldDrawItem> mDrawItems;
        std::unordered_map<uint64_t, WorldPose> mPreviousPoses;
        std::unordered_map<uint64_t, WorldPose> mCurrentPoses;
        std::vector<ViewCamera> mViewCameras;
        // mDrawItems plus one pass's view draws.
        std::vector<WorldDrawItem> mPassItems;
        // Parallel to mViewCameras.
        std::vector<std::vector<DrawItem>> mDrawItemsPerViewport;
        std::vector<RenderView> mRenderViews;
        std::vector<WorldDrawItem> mRayTracingWorldItems;
        std::vector<DrawItem> mRayTracingItems;
        std::vector<DrawItem> mSunShadowItems;
        std::array<std::vector<DrawItem>, VulkanRenderer::kMaxSpotShadows> mSpotShadowItems;
        std::array<std::array<std::vector<DrawItem>, 6>, VulkanRenderer::kMaxPointShadows> mPointShadowItems;

        bool mWarnedNoCamera = false;
        bool mWarnedNoActiveCamera = false;
        bool mWarnedNoRayTracing = false;
        bool mWarnedMultiViewRayTracing = false;
        bool mWasWireframeOnly = false;
        bool mWarnedSkippedItems = false;
    };
}
