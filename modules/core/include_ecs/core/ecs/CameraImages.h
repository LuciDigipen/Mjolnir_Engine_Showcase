/**
 * @file CameraImages.h
 * @author Quy Dong Ta
 * @brief The images cameras render into this frame instead of the screen, as a World resource, for what shows
 *        them, such as the game UI.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/ecs/Entity.h>
#include <core/ecs/PrimaryView.h>

#include <cstdint>
#include <vector>

namespace mir
{
    struct CameraImage
    {
        Entity mCamera;
        // the UI backend's texture for it, it changes when the image is resized
        uint64_t mUITexture = 0;
        // in pixels
        uint32_t mWidth = 0;
        uint32_t mHeight = 0;
        // the camera's view at the image's aspect, mWidth by mHeight
        PrimaryView mView;
    };

    // The renderer rewrites it last in LateUpdate, with the images it then draws
    struct CameraImages
    {
        std::vector<CameraImage> mImages;

        const CameraImage *Find(Entity camera) const
        {
            for (const CameraImage &image : mImages)
                if (image.mCamera == camera)
                    return &image;
            return nullptr;
        }
    };
}
