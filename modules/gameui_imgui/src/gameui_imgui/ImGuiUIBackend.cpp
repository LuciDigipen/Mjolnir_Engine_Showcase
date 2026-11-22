/**
 * @file ImGuiUIBackend.cpp
 * @author Quy Dong Ta
 * @brief Replays a UIDrawList into ImGui's background draw list, and keeps
 *        the game UI's textures as ImGui 1.92 user textures and its fonts in
 *        ImGui's atlas. Gradients and transforms are applied to the vertices
 *        each command wrote. The only file the game UI has that includes ImGui.
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

// ImGui's own font loader, set up as imgui_draw.cpp does, to try a font before the atlas asserts on it
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4505)
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#define STBTT_malloc(x, u) ((void)(u), IM_ALLOC(x))
#define STBTT_free(x, u) ((void)(u), IM_FREE(x))
#define STBTT_assert(x) do { IM_ASSERT(x); } while (0)
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
// its declarations only, which keep stb_truetype from declaring its own stbrp_node over imgui_internal.h's
#include <imstb_rectpack.h>
#include <imstb_truetype.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <algorithm>
#include <cfloat>
#include <climits>
#include <cmath>
#include <cstring>
#include <vector>

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

        // ImGui rounds a font size to whole pixels and asserts on 0, PushFont keeps it in this range the same way
        float PixelSize(float size)
        {
            return ImClamp(size, 1.0f, IMGUI_FONT_SIZE_MAX);
        }

        // the checks AddFontFromMemoryTTF makes on its first font, which it asserts on when they fail
        bool ImGuiCanLoad(std::span<const uint8_t> bytes)
        {
            const int offset = stbtt_GetFontOffsetForIndex(bytes.data(), 0);
            stbtt_fontinfo info{};
            return offset >= 0 && stbtt_InitFont(&info, bytes.data(), offset) != 0;
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

        ImVec2 ToScreen(UIPoint point, const UIRect &area, float scale)
        {
            return ImVec2(area.x + point.x * scale, area.y + point.y * scale);
        }

        UIPoint ToLocal(const ImVec2 &position, const UIRect &area, float scale)
        {
            return UIPoint{(position.x - area.x) / scale, (position.y - area.y) / scale};
        }

        // ImGui's own count can skip a short arc's start point, so an arc always gets both ends
        void ArcPath(ImDrawList &list, const ImVec2 &center, float radius, float from, float to)
        {
            const float turns = std::abs(to - from) / (2.0f * IM_PI);
            const int segments = static_cast<int>(std::ceil(list._CalcCircleAutoSegmentCount(radius) * turns));
            list.PathArcTo(center, radius, from, to, std::max(segments, 1));
        }

        // multiplies the colour of the vertices from first on by the gradient, read where each was drawn
        void Shade(ImDrawList &list, int first, const UIRect &area, float scale, const UIGradient &gradient)
        {
            const float dx = gradient.mTo.x - gradient.mFrom.x;
            const float dy = gradient.mTo.y - gradient.mFrom.y;
            const float length = dx * dx + dy * dy;
            const UIColor &from = gradient.mFromColor;
            const UIColor &to = gradient.mToColor;
            for (int i = first; i < list.VtxBuffer.Size; ++i)
            {
                ImDrawVert &vertex = list.VtxBuffer[i];
                const UIPoint local = ToLocal(vertex.pos, area, scale);
                const float along = ((local.x - gradient.mFrom.x) * dx + (local.y - gradient.mFrom.y) * dy) / length;
                const float t = std::clamp(along, 0.0f, 1.0f);
                ImVec4 color = ImGui::ColorConvertU32ToFloat4(vertex.col);
                color.x *= from.r + (to.r - from.r) * t;
                color.y *= from.g + (to.g - from.g) * t;
                color.z *= from.b + (to.b - from.b) * t;
                color.w *= from.a + (to.a - from.a) * t;
                vertex.col = ImGui::ColorConvertFloat4ToU32(color);
            }
        }

        // puts the vertices from first on through the transform, after shrinking text drawn textScale times too
        // big back about its origin
        void Move(ImDrawList &list, int first, const UIRect &area, float scale, const UITransform &transform,
                  UIPoint origin, float textScale)
        {
            for (int i = first; i < list.VtxBuffer.Size; ++i)
            {
                ImDrawVert &vertex = list.VtxBuffer[i];
                UIPoint local = ToLocal(vertex.pos, area, scale);
                if (textScale != 1.0f)
                    local = UIPoint{origin.x + (local.x - origin.x) / textScale,
                                    origin.y + (local.y - origin.y) / textScale};
                vertex.pos = ToScreen(transform.Apply(local), area, scale);
            }
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
        if (!ImGuiCanLoad(bytes))
        {
            MIR_LOG_ERROR("gameui: a {} byte font is damaged, ImGui cannot read its tables", bytes.size());
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
        std::vector<ImVec2> points;

        for (const UIDrawCommand &command : list.Commands())
        {
            drawList->PushClipRect(Min(command.mClip, area, scale), Max(command.mClip, area, scale), false);
            const int firstVertex = drawList->VtxBuffer.Size;
            const UITransform &transform = list.TransformOf(command);
            const ImU32 color = ToImColor(command.mColor);
            const ImVec2 center = ToScreen(UIPoint{command.mRect.x, command.mRect.y}, area, scale);
            const float radius = command.mRect.width * scale;
            float textScale = 1.0f;

            switch (command.mKind)
            {
            case UIDrawCommandKind::Rect:
                drawList->AddRectFilled(Min(command.mRect, area, scale), Max(command.mRect, area, scale), color,
                                        command.mRounding * scale);
                break;

            case UIDrawCommandKind::RectOutline:
                drawList->AddRect(Min(command.mRect, area, scale), Max(command.mRect, area, scale), color,
                                  command.mRounding * scale, 0, command.mThickness * scale);
                break;

            case UIDrawCommandKind::Texture:
            {
                auto *texture = static_cast<BackendTexture *>(textures.Find(command.mTexture));
                if (texture == nullptr)
                {
                    // a null texture, or one from another UIContext
                    drawList->AddRectFilled(Min(command.mRect, area, scale), Max(command.mRect, area, scale), color);
                    break;
                }

                const UIRect &uv = command.mUV;
                drawList->AddImage(texture->mData.GetTexRef(), Min(command.mRect, area, scale),
                                   Max(command.mRect, area, scale), ImVec2(uv.x, uv.y), ImVec2(uv.Right(), uv.Bottom()),
                                   color);
                break;
            }

            case UIDrawCommandKind::Text:
            {
                const std::string_view text = list.TextOf(command);
                ImFont *font = FontOf(fonts.Find(command.mFont));
                const float baseSize = command.mFontSize > 0.0f ? command.mFontSize : ImGui::GetFontSize();
                if (transform.IsIdentity())
                {
                    drawList->AddText(font, PixelSize(baseSize * scale), center, color, text.data(),
                                      text.data() + text.size());
                    break;
                }

                // drawn at the size it ends up and shrunk back by Move, so a scaled label stays sharp. Culled by
                // nothing here: the clip only knows where it lands once moved
                textScale = std::max(transform.AreaScale(), 0.01f);
                if ((color & IM_COL32_A_MASK) != 0)
                    font->RenderText(drawList, PixelSize(baseSize * scale * textScale), center, color,
                                     ImVec4(-FLT_MAX, -FLT_MAX, FLT_MAX, FLT_MAX), text.data(),
                                     text.data() + text.size());
                break;
            }

            case UIDrawCommandKind::Polyline:
            case UIDrawCommandKind::Polygon:
            {
                points.clear();
                for (const UIPoint &point : list.PointsOf(command))
                    points.push_back(ToScreen(point, area, scale));
                const int count = static_cast<int>(points.size());
                if (command.mKind == UIDrawCommandKind::Polygon)
                    drawList->AddConcavePolyFilled(points.data(), count, color);
                else
                    drawList->AddPolyline(points.data(), count, color,
                                          command.mClosed ? ImDrawFlags_Closed : ImDrawFlags_None,
                                          command.mThickness * scale);
                break;
            }

            case UIDrawCommandKind::Circle:
                if (command.mThickness > 0.0f)
                    drawList->AddCircle(center, radius, color, 0, command.mThickness * scale);
                else
                    drawList->AddCircleFilled(center, radius, color);
                break;

            case UIDrawCommandKind::Arc:
                if (command.mThickness > 0.0f)
                {
                    ArcPath(*drawList, center, radius, command.mAngleFrom, command.mAngleTo);
                    drawList->PathStroke(color, ImDrawFlags_None, command.mThickness * scale);
                }
                else
                {
                    drawList->PathLineTo(center);
                    ArcPath(*drawList, center, radius, command.mAngleFrom, command.mAngleTo);
                    drawList->PathFillConcave(color);
                }
                break;
            }

            if (const UIGradient *gradient = list.GradientOf(command))
                Shade(*drawList, firstVertex, area, scale, *gradient);
            if (!transform.IsIdentity())
                Move(*drawList, firstVertex, area, scale, transform, UIPoint{command.mRect.x, command.mRect.y},
                     textScale);

            drawList->PopClipRect();
        }
    }

    UISize ImGuiUIBackend::MeasureText(std::string_view text, float fontSize, void *font) const
    {
        ImFont *imFont = ImGui::GetCurrentContext() != nullptr ? FontOf(font) : nullptr;
        if (imFont == nullptr)
            return EstimateTextSize(text, fontSize > 0.0f ? fontSize : 16.0f);

        const float size = PixelSize(fontSize > 0.0f ? fontSize : ImGui::GetFontSize());
        const ImVec2 measured = imFont->CalcTextSizeA(size, FLT_MAX, 0.0f, text.data(), text.data() + text.size());
        return UISize{measured.x, measured.y};
    }
} // namespace mir
