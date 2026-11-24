/**
 * @file RenderTargetCameras.cpp
 * @author Quy Dong Ta
 * @brief Which cameras render into a RenderTexture, the render target each keeps from frame to frame, and the
 *        view each draws.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "renderer/RenderTargetCameras.h"
#include "renderer/ViewSubmission.h"

#include <core/ecs/World.h>
#include <core/ecs/components/WorldTransform.h>
#include <renderer/components/Camera.h>

#include <algorithm>

namespace mir
{
    VkExtent2D RenderTextureExtent(const RenderTexture &texture)
    {
        const auto side = [](int32_t value)
            { return static_cast<uint32_t>(std::clamp<int64_t>(value, 1, kMaxRenderTargetSize)); };
        return VkExtent2D{ side(texture.mWidth), side(texture.mHeight) };
    }

    std::vector<TargetCamera> SyncTargetCameras(World &world, const std::vector<TargetCamera> &current,
        const RenderTargetHost &host)
    {
        std::vector<TargetCamera> wanted;
        world.GetOrCreateQuery<const WorldTransform, const Camera, const RenderTexture>().ForEach(
            [&](Entity entity, const WorldTransform &, const Camera &, const RenderTexture &texture)
            { wanted.push_back({ .mCamera = entity, .mExtent = RenderTextureExtent(texture) }); });

        const auto find = [](const std::vector<TargetCamera> &cameras, Entity entity)
        {
            return std::find_if(cameras.begin(), cameras.end(),
                [&](const TargetCamera &camera) { return camera.mCamera == entity; });
        };
        for (const TargetCamera &camera : current)
            if (!camera.mTarget.IsNull() && find(wanted, camera.mCamera) == wanted.end())
                host.mRelease(camera.mTarget);

        const auto create = [&](TargetCamera &camera)
        {
            camera.mTarget = host.mCreate(camera.mExtent);
            camera.mLiveWhenRefused = camera.mTarget.IsNull() ? host.mLiveCount() : 0;
        };
        for (TargetCamera &want : wanted)
        {
            const auto found = find(current, want.mCamera);
            if (found == current.end())
            {
                create(want);
                continue;
            }
            const bool resized = found->mExtent.width != want.mExtent.width ||
                found->mExtent.height != want.mExtent.height;
            if (found->mTarget.IsNull())
            {
                if (resized || host.mLiveCount() < found->mLiveWhenRefused)
                    create(want);
                else
                    want = *found;
                continue;
            }
            want.mTarget = found->mTarget;
            if (resized && !host.mResize(want.mTarget, want.mExtent))
                want.mExtent = found->mExtent;
        }
        return wanted;
    }

    std::optional<ViewCamera> ResolveTargetCamera(World &world, const TargetCamera &camera)
    {
        const WorldTransform *transform = world.GetComponent<WorldTransform>(camera.mCamera);
        const Camera *lens = world.GetComponent<Camera>(camera.mCamera);
        if (transform == nullptr || lens == nullptr || camera.mTarget.IsNull())
            return std::nullopt;

        ViewCamera view{ .viewport = kFirstTargetViewport + camera.mTarget.mIndex, .pose = transform->Pose(),
            .lens = *lens, .extent = camera.mExtent };
        view.viewProj = MakeCameraRelativeViewProjection(view.pose, view.lens,
            static_cast<float>(camera.mExtent.width) / static_cast<float>(camera.mExtent.height));
        return view;
    }
}
