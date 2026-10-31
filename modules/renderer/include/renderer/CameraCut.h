/**
 * @file CameraCut.h
 * @author Quy Dong Ta
 * @brief Camera cuts: whoever jumps a camera says so, and RenderSystem draws
 *        that camera's next frame with no temporal history.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/ecs/Entity.h>

#include <vector>

namespace mir
{
    class World;

    // Cameras cut since the last frame RenderSystem drew, which clears it each frame.
    struct CameraCuts
    {
        std::vector<Entity> mCameras;
    };

    // False if camera has no Camera.
    bool CutCamera(World &world, Entity camera);

    bool IsCameraCut(const World &world, Entity camera);

    void ClearCameraCuts(World &world);
}
