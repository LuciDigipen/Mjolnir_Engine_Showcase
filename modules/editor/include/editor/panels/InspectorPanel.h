/**
 * @file InspectorPanel.h
 * @author Rahul Nair
 * @brief Shows and edits the Selection's components - the editor's
 *        "Inspector" dock panel.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/ecs/World.h>
#include <editor/SnapToTerrain.h>
#include <editor/panels/AssetImportPanel.h>

#include <cstddef>
#include <vector>

namespace mir
{
    struct ComponentOps;
    class History;
    class IAuthoredEntities;

    class InspectorPanel
    {
    public:
        
        // True when Snap to Terrain was clicked from the Transform menu.
        bool Draw(World &world, const AssetImportView &assets, History &history,
            IAuthoredEntities *authored, const SnapToTerrainAction &snap);

        // One field's value
        struct alignas(16) FieldBuffer
        {
            std::byte mBytes[64];
        };

        // A component's values copied field by field, so pasting runs each field's setter
        struct Clipboard
        {
            const ComponentOps *mOps = nullptr;
            std::vector<FieldBuffer> mValues; // one per mOps->mFields, in order
        };

    private:
        
        void EndEditIfReleased(History &history);

        Clipboard mClipboard;
        bool mEditing = false;
        char mFilter[128] = {};
    };
} // namespace mir
