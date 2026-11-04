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

namespace mir
{
    // The background list renders before every ImGui window, and the renderer draws
    // ImGui after the 3D views, so the game UI lands over the scene and under the
    // editor's panels. Submit runs inside an ImGui frame
    class ImGuiUIBackend final : public IUIBackend
    {
    public:
        UIBackendTexture CreateTexture(uint32_t width, uint32_t height, std::span<const uint8_t> rgba) override;
        void Submit(const UIRect &area, float scale, const UIDrawList &list, const UITextures &textures) override;
        // with ImGui's font; without an ImGui context, an estimate
        UISize MeasureText(std::string_view text, float fontSize) const override;
    };
}
