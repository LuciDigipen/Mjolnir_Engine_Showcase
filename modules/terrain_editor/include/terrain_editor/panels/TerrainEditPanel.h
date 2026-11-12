/**
 * @file TerrainEditPanel.h
 * @author Jackey Cen
 * @brief Renders the editor's "Terrain" panel: edit layers, brush settings and Bake.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

namespace mir
{
    class TerrainEditMode;

    class TerrainEditPanel
    {
    public:
        void Draw(TerrainEditMode &mode);
    };
}
