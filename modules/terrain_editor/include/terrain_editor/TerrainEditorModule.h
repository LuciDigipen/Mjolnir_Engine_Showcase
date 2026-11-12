/**
 * @file TerrainEditorModule.h
 * @author Jackey Cen
 * @brief Terrain editing, registered with the editor as an extension.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <terrain_editor/TerrainEditMode.h>
#include <terrain_editor/TerrainLayerWorkspace.h>
#include <terrain_editor/panels/TerrainEditPanel.h>

#include <core/Registration.h>
#include <core/ecs/ModuleContext.h>

#include <memory>

namespace mir
{
    class EditorModule;
    struct GameProject;

    class TerrainEditorModule
    {
    public:
        TerrainEditorModule(ModuleContext context, EditorModule &editor, const GameProject &project);
        ~TerrainEditorModule();

        TerrainEditorModule(const TerrainEditorModule &) = delete;
        TerrainEditorModule &operator=(const TerrainEditorModule &) = delete;

    private:
        class Extension;

        TerrainEditMode mMode;
        TerrainEditPanel mPanel;
        TerrainLayerWorkspace mWorkspace;
        std::unique_ptr<Extension> mExtension;
        Registration mRegistration;
    };
} // namespace mir
