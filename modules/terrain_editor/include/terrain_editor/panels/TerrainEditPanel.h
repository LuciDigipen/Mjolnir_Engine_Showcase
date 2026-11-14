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
#include <span>
#include <string>

namespace mir
{
    class History;
    class TerrainEditMode;
    class TerrainLayerDocument;
    class TerrainLayerWorkspace;
    struct TerrainBrushAvailability;

    struct TerrainBakeView
    {
        bool mBaking = false;
        bool mCanStart = false;
        bool mCanCancel = false;
        bool mFailed = false;
        double mSeconds = 0.0;
        std::string mStatus;
        std::string mReason;
    };

    struct TerrainBakeActions
    {
        bool mStart = false;
        bool mCancel = false;
    };

    class TerrainEditPanel
    {
    public:
        TerrainBakeActions Draw(TerrainEditMode &mode, TerrainLayerWorkspace &layers, History &history,
                                const TerrainBrushAvailability &brush, const TerrainBakeView &bake,
                                std::span<const std::string> materialLayers, std::string &chosenMaterial);

    private:
        void DrawLayers(TerrainLayerWorkspace &layers, History &history);
        void DrawLayerTable(TerrainLayerDocument &document, TerrainLayerWorkspace &layers, History &history);

        TerrainEditBlend mNewLayerBlend = TerrainEditBlend::Additive;
        std::optional<uint32_t> mRenameSequence;
        char mRenameBuffer[128] = {};
        std::string mRenameError;
    };
} // namespace mir
