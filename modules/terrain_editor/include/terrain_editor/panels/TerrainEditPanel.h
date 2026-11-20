/**
 * @file TerrainEditPanel.h
 * @author Jackey Cen
 * @brief Renders the editor's "Terrain" panel: edit layers, brush settings, material layers and Bake.
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
    class TerrainMaterialLayersDraft;
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

    struct TerrainMaterialLayersView
    {
        // Null while no terrain asset is selected or the draft did not load.
        TerrainMaterialLayersDraft *mDraft = nullptr;
        // Surface materials under the assets root whose manifest declares the terrain frontend.
        std::span<const std::string> mMaterials;
        std::span<const std::string> mPaintedLayers;
        std::string mLoadError;
        std::string mStatus;
        std::string mDisabledReason;
        bool mFailed = false;
        bool mCanApply = false;
    };

    struct TerrainPanelActions
    {
        bool mStartBake = false;
        bool mCancelBake = false;
        bool mApplyMaterials = false;
        bool mRevertMaterials = false;
        bool mRescanMaterials = false;
    };

    class TerrainEditPanel
    {
    public:
        TerrainPanelActions Draw(TerrainEditMode &mode, TerrainLayerWorkspace &layers, History &history,
                                 const TerrainBrushAvailability &brush, const TerrainBakeView &bake,
                                 const TerrainMaterialLayersView &materialLayers,
                                 std::span<const std::string> materialLayerNames, std::string &chosenMaterial);

    private:
        void DrawLayers(TerrainLayerWorkspace &layers, History &history);
        void DrawLayerTable(TerrainLayerDocument &document, TerrainLayerWorkspace &layers, History &history);
        void DrawMaterialLayers(const TerrainMaterialLayersView &view, TerrainPanelActions &actions);

        TerrainEditBlend mNewLayerBlend = TerrainEditBlend::Additive;
        std::optional<uint32_t> mRenameSequence;
        char mRenameBuffer[128] = {};
        std::string mRenameError;

        bool mMaterialSectionOpen = false;
        const TerrainMaterialLayersDraft *mMaterialDraftSeen = nullptr;
        std::string mMaterialRecipeSeen;
        int mMaterialRenameRow = -1;
        char mMaterialRenameBuffer[128] = {};
        std::string mMaterialRenameError;
        int mMaterialDeleteRow = -1;
    };
} // namespace mir
