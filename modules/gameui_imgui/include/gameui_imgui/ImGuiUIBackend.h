/**
 * @file ImGuiUIBackend.h
 * @author sumin.park
 * @brief gameui's backend over ImGui's background draw list and its 1.92 user textures.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <gameui/UIBackend.h>

#include <cstddef>

namespace mir
{
    // The background list renders before every ImGui window, and the renderer draws
    // ImGui after the 3D views, so the game UI lands over the scene and under the
    // editor's panels. Submit runs inside an ImGui frame
    class ImGuiUIBackend final : public IUIBackend
    {
    public:
        UIBackendTexture CreateTexture(uint32_t width, uint32_t height, std::span<const uint8_t> rgba) override;
        // a font in ImGui's atlas, which loads its glyphs at each size as text first uses it
        UIBackendFont MakeFont(std::span<const uint8_t> bytes) override;
        void Submit(const UIRect &area, float scale, const UIDrawList &list, const UITextures &textures,
                    const UIFonts &fonts) override;
        
        UISize MeasureText(std::string_view text, float fontSize, void *font) const override;

        // A texture uploaded when it was released, such as a Play session's when Play stops, is destroyed by ImGui's
        // renderer backend once no frame in flight uses it. This frees each one that is, and gives how many still
        // wait. Every frame, inside the ImGui context
        static std::size_t RetireTextures();
    };
}
