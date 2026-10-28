/**
 * @file HierarchyPanel.cpp
 * @author Rahul Nair
 * @brief Renders the World's entities as a tree - the editor's "Hierarchy" dock panel.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/ecs/Entity.h>

#include <imgui.h>

#include <cstdint>
#include <cstring>
#include <optional>

namespace mir
{
    inline constexpr const char *kEntityPayload = "MIR_HIERARCHY_ENTITIES";

    // One packed uint64_t identifies the row that started the drag, including
    // when multiple entities are selected. The Hierarchy keeps the group to
    // move separately; consumers of a single reference use this source row.
    // Consumers must check that the decoded handle is alive in their World.
    inline std::optional<Entity> DecodeEntityDragPayload(const ImGuiPayload &payload)
    {
        if (!payload.IsDataType(kEntityPayload) || payload.Data == nullptr ||
            payload.DataSize != sizeof(uint64_t))
            return std::nullopt;

        uint64_t packed;
        std::memcpy(&packed, payload.Data, sizeof(packed));
        const Entity entity = UnpackEntity(packed);
        return entity.IsNull() ? std::nullopt : std::optional<Entity>{ entity };
    }
}
