/**
 * @file ImGuiUIBackend.cpp
 * @author Quy Dong Ta
 * @brief Replays a UIDrawList into ImGui's background draw list, and keeps
 *        the game UI's textures as ImGui 1.92 user textures and its fonts in
 *        ImGui's atlas. The only file the game UI has that includes ImGui.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <gameui_imgui/ImGuiUIBackend.h>

#include <gameui/UIDrawList.h>
#include <gameui/UIFonts.h>
#include <gameui/UITextures.h>

#include <core/log/Log.h>

#include <imgui.h>
// RegisterUserTexture and UnregisterUserTexture
#include <imgui_internal.h>

#include <cfloat>
#include <climits>
#include <cstring>

namespace mir
{
    namespace
    {
        struct BackendTexture
        {
            ImTextureData mData;
            ImGuiContext *mContext = nullptr;
        };

        struct BackendFont
        {
            ImFont *mFont = nullptr;
            ImGuiContext *mContext = nullptr;
        };

        // a context destroyed first took the font and its bytes down with its atlas
        void ReleaseFont(void *opaque)
        {
            auto *font = static_cast<BackendFont *>(opaque);
            if (font == nullptr)
                return;
            if (ImGui::GetCurrentContext() == font->mContext)
                ImGui::GetIO().Fonts->RemoveFont(font->mFont);
            delete font;
        }

        ImFont *FontOf(void *font)
        {
            return font != nullptr ? static_cast<BackendFont *>(font)->mFont : ImGui::GetFont();
        }

        ImU32 ToImColor(UIColor color) { return ImGui::ColorConvertFloat4ToU32(ImVec4(color.r, color.g, color.b, color.a)); }

        ImVec2 Min(const UIRect &rect, const UIRect &area, float scale)
        {
            return ImVec2(area.x + rect.x * scale, area.y + rect.y * scale);
        }
        ImVec2 Max(const UIRect &rect, const UIRect &area, float scale)
        {
            return ImVec2(area.x + rect.Right() * scale, area.y + rect.Bottom() * scale);
        }

        // once ImGui has shut down its renderer backend freed the GPU side, so only
        // the memory is left; while it runs, only a copy never uploaded can go
        void Release(void *opaque)
        {
            auto *texture = static_cast<BackendTexture *>(opaque);
            if (texture == nullptr)
                return;

            if (ImGui::GetCurrentContext() != texture->mContext)
            {
                delete texture;
                return;
            }

            ImTextureData &data = texture->mData;
            if (data.TexID != ImTextureID_Invalid || data.BackendUserData != nullptr)
            {
                MIR_LOG_ERROR("gameui: a UI texture outlived by its renderer backend is leaked; "
                              "destroy the game UI after ImGui shuts down");
                return;
            }

            ImGui::UnregisterUserTexture(&data);
            ImGui::GetPlatformIO().Textures.find_erase(&data);
            delete texture;
        }
    }

    UIBackendTexture ImGuiUIBackend::CreateTexture(uint32_t width, uint32_t height, std::span<const uint8_t> rgba)
    {
        if (ImGui::GetCurrentContext() == nullptr)
        {
            MIR_LOG_ERROR("gameui: a UI texture needs an ImGui context");
            return {};
        }

        auto *texture = new BackendTexture;
        ImTextureData &data = texture->mData;
        data.Create(ImTextureFormat_RGBA32, static_cast<int>(width), static_cast<int>(height));
        std::memcpy(data.GetPixels(), rgba.data(), rgba.size());
        data.UseColors = true;
        data.UsedRect = ImTextureRect{0, 0, static_cast<unsigned short>(width), static_cast<unsigned short>(height)};

        ImGui::RegisterUserTexture(&data);
        texture->mContext = ImGui::GetCurrentContext();
        return UIBackendTexture{ texture, &Release };
    }

    UIBackendFont ImGuiUIBackend::MakeFont(std::span<const uint8_t> bytes)
    {
        if (ImGui::GetCurrentContext() == nullptr)
        {
            MIR_LOG_ERROR("gameui: a UI font needs an ImGui context");
            return {};
        }
        if (bytes.size() > static_cast<size_t>(INT_MAX))
        {
            MIR_LOG_ERROR("gameui: a {} byte font is too large", bytes.size());
            return {};
        }

        // the atlas owns the copy and frees it with the font, or at once when it does not load
        void *copy = IM_ALLOC(bytes.size());
        std::memcpy(copy, bytes.data(), bytes.size());
        ImFont *font = ImGui::GetIO().Fonts->AddFontFromMemoryTTF(copy, static_cast<int>(bytes.size()), 16.0f);
        if (font == nullptr)
        {
            MIR_LOG_ERROR("gameui: ImGui could not load a {} byte font", bytes.size());
            return {};
        }

        return UIBackendFont{new BackendFont{font, ImGui::GetCurrentContext()}, &ReleaseFont};
    }

    void ImGuiUIBackend::Submit(const UIRect &area, float scale, const UIDrawList &list, const UITextures &textures,
                                const UIFonts &fonts)
    {
        ImDrawList *drawList = ImGui::GetBackgroundDrawList();

        for (const UIDrawCommand &command : list.Commands())
        {
            drawList->PushClipRect(Min(command.mClip, area, scale), Max(command.mClip, area, scale), false);

            switch (command.mKind)
            {
            case UIDrawCommandKind::Rect:
                drawList->AddRectFilled(Min(command.mRect, area, scale), Max(command.mRect, area, scale),
                                        ToImColor(command.mColor), command.mRounding * scale);
                break;

            case UIDrawCommandKind::Texture:
            {
                auto *texture = static_cast<BackendTexture *>(textures.Find(command.mTexture));
                if (texture == nullptr)
                {
                    // a null texture, or one from another UIContext
                    drawList->AddRectFilled(Min(command.mRect, area, scale), Max(command.mRect, area, scale),
                                            ToImColor(command.mColor));
                    break;
                }

                const UIRect &uv = command.mUV;
                drawList->AddImage(texture->mData.GetTexRef(), Min(command.mRect, area, scale),
                                   Max(command.mRect, area, scale), ImVec2(uv.x, uv.y), ImVec2(uv.Right(), uv.Bottom()),
                                   ToImColor(command.mColor));
                break;
            }

            case UIDrawCommandKind::Text:
            {
                const std::string_view text = list.TextOf(command);
                const float fontSize = (command.mFontSize > 0.0f ? command.mFontSize : ImGui::GetFontSize()) * scale;
                drawList->AddText(FontOf(fonts.Find(command.mFont)), fontSize, Min(command.mRect, area, scale),
                                  ToImColor(command.mColor), text.data(), text.data() + text.size());
                break;
            }
            }

            drawList->PopClipRect();
        }
    }

    UISize ImGuiUIBackend::MeasureText(std::string_view text, float fontSize, void *font) const
    {
        ImFont *imFont = ImGui::GetCurrentContext() != nullptr ? FontOf(font) : nullptr;
        if (imFont == nullptr)
            return EstimateTextSize(text, fontSize > 0.0f ? fontSize : 16.0f);

        const float size = fontSize > 0.0f ? fontSize : ImGui::GetFontSize();
        const ImVec2 measured = imFont->CalcTextSizeA(size, FLT_MAX, 0.0f, text.data(), text.data() + text.size());
        return UISize{measured.x, measured.y};
    }
} // namespace mir
