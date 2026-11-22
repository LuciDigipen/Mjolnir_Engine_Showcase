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
#include <string_view>

namespace mir
{
    class UIDrawList;
    class UIFonts;
    class UITextures;

    // A backend's copy of one texture. It releases itself through release, so it needs
    // no backend object alive: ImGui's renderer backend frees an uploaded copy's GPU
    // side at its own shutdown, and the copy must outlive that
    struct UIBackendTexture
    {
        void *texture = nullptr;
        void (*release)(void *texture) = nullptr;
    };

    // A backend's copy of one font, released the same way as a texture
    struct UIBackendFont
    {
        void *font = nullptr;
        void (*release)(void *font) = nullptr;
    };

    // text's size with no font to ask: half the font size a character, one line the font size tall
    inline UISize EstimateTextSize(std::string_view text, float fontSize)
    {
        return UISize{0.5f * fontSize * static_cast<float>(text.size()), fontSize};
    }

    // gameui_imgui implements it over ImGui's background draw list
    class IUIBackend
    {
    public:
        virtual ~IUIBackend() = default;

        // width x height RGBA8 pixels, an empty one when it cannot be made (logged)
        virtual UIBackendTexture CreateTexture(uint32_t width, uint32_t height, std::span<const uint8_t> rgba) = 0;

        // a TrueType or OpenType file's bytes, an empty one when it cannot be made (logged).
        // Not CreateFont, which windows.h defines as a macro
        virtual UIBackendFont MakeFont(std::span<const uint8_t> bytes) = 0;

        // one frame's list, into area, over the scene, the list is in UI units, scale area pixels each
        virtual void Submit(const UIRect &area, float scale, const UIDrawList &list, const UITextures &textures,
                            const UIFonts &fonts) = 0;

        // @p text's size in pixels drawn at @p fontSize pixels in @p font, a font this backend made or null for its
        // default, a size of 0 is the backend's own
        virtual UISize MeasureText(std::string_view text, float fontSize, void *font) const = 0;
    };
}
