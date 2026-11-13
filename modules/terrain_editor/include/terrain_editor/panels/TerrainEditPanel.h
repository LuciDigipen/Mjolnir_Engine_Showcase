/**
 * @file TerrainEditPanel.h
 * @author Jackey Cen
 * @brief Renders the editor's "Terrain" panel: edit layers, brush settings and Bake.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <terrain/TerrainEditLayer.h>

#include <cstdint>
#include <optional>
#include <string>

namespace mir
{
    class History;
    class TerrainEditMode;
    class TerrainLayerDocument;
    class TerrainLayerWorkspace;
    struct TerrainBrushAvailability;

    class TerrainEditPanel
    {
    public:
        void Draw(TerrainEditMode &mode, TerrainLayerWorkspace &layers, History &history,
                  const TerrainBrushAvailability &brush);

    private:
        void DrawLayers(TerrainLayerWorkspace &layers, History &history);
        void DrawLayerTable(TerrainLayerDocument &document, TerrainLayerWorkspace &layers, History &history);

        TerrainEditBlend mNewLayerBlend = TerrainEditBlend::Additive;
        std::optional<uint32_t> mRenameSequence;
        char mRenameBuffer[128] = {};
        std::string mRenameError;
    };
} // namespace mir
