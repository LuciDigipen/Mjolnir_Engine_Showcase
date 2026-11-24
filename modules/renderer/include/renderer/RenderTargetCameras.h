/**
 * @file RenderTargetCameras.h
 * @author Quy Dong Ta
 * @brief Which cameras render into a RenderTexture, the render target each keeps from frame to frame, and the
 *        view each draws.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/ecs/Entity.h>
#include <renderer/RenderTarget.h>
#include <renderer/ViewDrawSource.h>
#include <renderer/components/RenderTexture.h>

#include <volk.h>

#include <cstddef>
#include <functional>
#include <optional>
#include <vector>

namespace mir
{
    class World;

    // A target camera's viewport id is this plus its target's index, apart from RenderSystem's viewports and
    // the same while the target lives, as view draw sources key their state by it
    inline constexpr size_t kFirstTargetViewport = size_t(1) << 20;

    struct TargetCamera
    {
        Entity mCamera;
        // null when the renderer could not make one, tried again once the size changes or a target is released
        RenderTargetHandle mTarget;
        VkExtent2D mExtent{};
        // the renderer's live targets when it could not make one
        uint32_t mLiveWhenRefused = 0;
    };

    struct RenderTargetHost
    {
        std::function<RenderTargetHandle(VkExtent2D)> mCreate;
        std::function<bool(RenderTargetHandle, VkExtent2D)> mResize;
        std::function<void(RenderTargetHandle)> mRelease;
        std::function<uint32_t()> mLiveCount;
    };

    // Each side within 1 to kMaxRenderTargetSize
    VkExtent2D RenderTextureExtent(const RenderTexture &texture);

    // The RenderTexture cameras of @p world, in its order, each with the target it had in @p current, a new one for
    // a camera new to it, resized when its size changed. The targets of cameras gone are released first, so a
    // camera without one can have their slots
    std::vector<TargetCamera> SyncTargetCameras(World &world, const std::vector<TargetCamera> &current,
        const RenderTargetHost &host);

    // The camera's view at its image's aspect, none once it lacks a Camera or a WorldTransform
    std::optional<ViewCamera> ResolveTargetCamera(World &world, const TargetCamera &camera);
}
