/**
 * @file CameraCut.cpp
 * @author Quy Dong Ta
 * @brief Camera cuts: whoever jumps a camera says so, and RenderSystem draws
 *        that camera's next frame with no temporal history.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "renderer/CameraCut.h"

#include <core/ecs/World.h>
#include <renderer/components/Camera.h>

#include <algorithm>

namespace mir
{
    bool CutCamera(World &world, Entity camera)
    {
        if (world.GetComponent<Camera>(camera) == nullptr)
            return false;

        CameraCuts *cuts = world.TryResource<CameraCuts>();
        if (cuts == nullptr)
            cuts = &world.EmplaceResource<CameraCuts>();
        if (std::find(cuts->mCameras.begin(), cuts->mCameras.end(), camera) == cuts->mCameras.end())
            cuts->mCameras.push_back(camera);
        return true;
    }

    bool IsCameraCut(const World &world, Entity camera)
    {
        const CameraCuts *cuts = world.TryResource<CameraCuts>();
        return cuts != nullptr && !camera.IsNull() &&
            std::find(cuts->mCameras.begin(), cuts->mCameras.end(), camera) != cuts->mCameras.end();
    }

    void ClearCameraCuts(World &world)
    {
        if (CameraCuts *cuts = world.TryResource<CameraCuts>())
            cuts->mCameras.clear();
    }
}
