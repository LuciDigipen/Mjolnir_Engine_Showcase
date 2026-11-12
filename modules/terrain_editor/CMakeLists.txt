# not part of a build without the editor (MJOLNIR_WITH_EDITOR OFF)
if(NOT MJOLNIR_WITH_EDITOR)
    return()
endif()

engine_add_module(terrain_editor)

target_sources(engine_terrain_editor PRIVATE
    src/terrain_editor/TerrainEditMode.cpp
    src/terrain_editor/TerrainEditorModule.cpp
    src/terrain_editor/panels/TerrainEditPanel.cpp
)

# PUBLIC: TerrainEditorModule.h takes an EditorModule and holds the edit mode and its panel
target_link_libraries(engine_terrain_editor
    PUBLIC mir::editor
           mir::terrain
    PRIVATE imgui::imgui
            iconfontcppheaders::iconfontcppheaders
)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
