# not part of a build without the editor (MJOLNIR_WITH_EDITOR OFF)
if(NOT MJOLNIR_WITH_EDITOR)
    return()
endif()

engine_add_module(terrain_editor)

find_package(nlohmann_json CONFIG REQUIRED)

target_sources(engine_terrain_editor PRIVATE
    src/terrain_editor/TerrainBake.cpp
    src/terrain_editor/TerrainBakeJob.cpp
    src/terrain_editor/TerrainEditBase.cpp
    src/terrain_editor/TerrainEditMode.cpp
    src/terrain_editor/TerrainEditOverlay.cpp
    src/terrain_editor/TerrainEditorModule.cpp
    src/terrain_editor/TerrainHeightBrush.cpp
    src/terrain_editor/TerrainLayerCommands.cpp
    src/terrain_editor/TerrainLayerDocument.cpp
    src/terrain_editor/TerrainLayerWorkspace.cpp
    src/terrain_editor/TerrainPaintBrush.cpp
    src/terrain_editor/panels/TerrainEditPanel.cpp
)

# PUBLIC: TerrainEditorModule.h takes an EditorModule and the GameProject its assets come from
target_link_libraries(engine_terrain_editor
    PUBLIC mir::editor
           mir::terrain
    PRIVATE imgui::imgui
            iconfontcppheaders::iconfontcppheaders
            mir::project
            nlohmann_json::nlohmann_json
)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
