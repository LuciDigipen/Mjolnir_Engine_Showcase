/**
 * @file ViewPie.h
 * @author Rahul Nair
 * @brief The held-Z pie over the scene viewport that picks its view mode -
 *        which wedge the mouse points at and what choosing it changes.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <renderer/DebugView.h>

#include <glm/vec2.hpp>

#include <cstdint>
#include <optional>

namespace mir
{
    enum class ViewPieItem : uint8_t
    {
        Render,
        Wireframe,
        RenderWireframe,
        Colliders,
    };

    inline constexpr ViewPieItem kViewPieItems[] = {
        ViewPieItem::Render, ViewPieItem::Wireframe, ViewPieItem::RenderWireframe, ViewPieItem::Colliders };

    // offset is the mouse minus the pie's center in screen pixels, y down: right is Render, left Wireframe,
    // up Render + Wireframe, down Colliders. nullopt inside deadZone. A diagonal goes to the horizontal wedge.
    std::optional<ViewPieItem> ViewPieItemAt(glm::vec2 offset, float deadZone);

    // Unit screen direction from the pie's center to the item's wedge, y down.
    glm::vec2 ViewPieDirection(ViewPieItem item);

    const char *ViewPieLabel(ViewPieItem item);

    // A shading item replaces the shading; Colliders toggles over whichever shading is on.
    void ApplyViewPieItem(DebugViewSettings &view, ViewPieItem item);

    // Whether the item shows as chosen: the current shading, or Colliders while they are on.
    bool ViewPieItemActive(const DebugViewSettings &view, ViewPieItem item);
}
