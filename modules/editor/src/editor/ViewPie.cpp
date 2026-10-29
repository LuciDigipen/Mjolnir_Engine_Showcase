/**
 * @file ViewPie.cpp
 * @author Rahul Nair
 * @brief The held-Z pie over the scene viewport that picks its view mode -
 *        which wedge the mouse points at and what choosing it changes.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <editor/ViewPie.h>

#include <cmath>

namespace mir
{
    std::optional<ViewPieItem> ViewPieItemAt(glm::vec2 offset, float deadZone)
    {
        if (offset.x * offset.x + offset.y * offset.y <= deadZone * deadZone)
            return std::nullopt;

        if (std::abs(offset.x) >= std::abs(offset.y))
            return offset.x < 0.0f ? ViewPieItem::Wireframe : ViewPieItem::Render;
        return offset.y < 0.0f ? ViewPieItem::RenderWireframe : ViewPieItem::Colliders;
    }

    glm::vec2 ViewPieDirection(ViewPieItem item)
    {
        switch (item)
        {
        case ViewPieItem::Render:
            return { 1.0f, 0.0f };
        case ViewPieItem::Wireframe:
            return { -1.0f, 0.0f };
        case ViewPieItem::RenderWireframe:
            return { 0.0f, -1.0f };
        case ViewPieItem::Colliders:
            return { 0.0f, 1.0f };
        }
        return { 0.0f, 0.0f };
    }

    const char *ViewPieLabel(ViewPieItem item)
    {
        switch (item)
        {
        case ViewPieItem::Render:
            return "Render";
        case ViewPieItem::Wireframe:
            return "Wireframe";
        case ViewPieItem::RenderWireframe:
            return "Render + Wireframe";
        case ViewPieItem::Colliders:
            return "Colliders";
        }
        return "";
    }

    void ApplyViewPieItem(DebugViewSettings &view, ViewPieItem item)
    {
        switch (item)
        {
        case ViewPieItem::Render:
            view.shading = ViewportShading::Render;
            break;
        case ViewPieItem::Wireframe:
            view.shading = ViewportShading::Wireframe;
            break;
        case ViewPieItem::RenderWireframe:
            view.shading = ViewportShading::RenderWireframe;
            break;
        case ViewPieItem::Colliders:
            view.colliders = !view.colliders;
            break;
        }
    }

    bool ViewPieItemActive(const DebugViewSettings &view, ViewPieItem item)
    {
        switch (item)
        {
        case ViewPieItem::Render:
            return view.shading == ViewportShading::Render;
        case ViewPieItem::Wireframe:
            return view.shading == ViewportShading::Wireframe;
        case ViewPieItem::RenderWireframe:
            return view.shading == ViewportShading::RenderWireframe;
        case ViewPieItem::Colliders:
            return view.colliders;
        }
        return false;
    }
}
