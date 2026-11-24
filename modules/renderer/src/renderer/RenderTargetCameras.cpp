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
        std::vector<TargetCamera> kept;
        std::vector<bool> claimed(current.size(), false);
        world.GetOrCreateQuery<const WorldTransform, const Camera, const RenderTexture>().ForEach(
            [&](Entity entity, const WorldTransform &, const Camera &, const RenderTexture &texture)
            {
                const VkExtent2D extent = RenderTextureExtent(texture);
                const auto found = std::find_if(current.begin(), current.end(),
                    [&](const TargetCamera &camera) { return camera.mCamera == entity; });
                if (found == current.end())
                {
                    kept.push_back({ entity, host.mCreate(extent), extent });
                    return;
                }
                claimed[size_t(found - current.begin())] = true;
                TargetCamera camera = *found;
                if (camera.mExtent.width == extent.width && camera.mExtent.height == extent.height)
                {
                    kept.push_back(camera);
                    return;
                }
                if (camera.mTarget.IsNull())
                {
                    camera.mTarget = host.mCreate(extent);
                    camera.mExtent = extent;
                }
                else if (host.mResize(camera.mTarget, extent))
                    camera.mExtent = extent;
                kept.push_back(camera);
            });

        for (size_t i = 0; i < current.size(); ++i)
            if (!claimed[i] && !current[i].mTarget.IsNull())
                host.mRelease(current[i].mTarget);
        return kept;
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
