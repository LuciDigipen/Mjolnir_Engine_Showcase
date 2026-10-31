/**
 * @file UIBackend.h
 * @author sumin.park
 * @brief How the game UI reaches the screen; gameui itself names no graphics API.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "UITypes.h"

#include <cstdint>
#include <span>

namespace mir
{
    class UIDrawList;
    class UITextures;

    // A backend's copy of one texture. It releases itself through release, so it needs
    // no backend object alive: ImGui's renderer backend frees an uploaded copy's GPU
    // side at its own shutdown, and the copy must outlive that
    struct UIBackendTexture
    {
        void *texture = nullptr;
        void (*release)(void *texture) = nullptr;
    };

    // gameui_imgui implements it over ImGui's background draw list
    class IUIBackend
    {
    public:
        virtual ~IUIBackend() = default;

        // width x height RGBA8 pixels, an empty one when it cannot be made (logged)
        virtual UIBackendTexture CreateTexture(uint32_t width, uint32_t height, std::span<const uint8_t> rgba) = 0;

        // one frame's list, into area, over the scene, the list is in UI units, scale area pixels each
        virtual void Submit(const UIRect &area, float scale, const UIDrawList &list, const UITextures &textures) = 0;
    };
}
