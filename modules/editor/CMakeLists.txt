# not part of a build without the editor (MJOLNIR_WITH_EDITOR OFF)
if(NOT MJOLNIR_WITH_EDITOR)
    return()
endif()

engine_add_module(editor)

find_package(Stb REQUIRED)
target_include_directories(engine_editor PRIVATE ${Stb_INCLUDE_DIR})

target_link_libraries(engine_editor
    # PUBLIC core: EditorModule.h holds Registrations and takes a ModuleContext
    PUBLIC mir::core
           mir::imguiruntime
           mir::renderer
           mir::window
           mir::input
           mir::project
           # ProjectSettingsPanel.h holds a PhysicsLayerTable
           mir::physics_layers
           # PrefabWorkspace.h and PrefabAuthoringAdapter.h take prefab types
           mir::prefab
    PRIVATE mir::animation
            mir::assets
            mir::editortheme
            mir::gameui
            mir::scene
            mir::serial
            imgui::imgui
            imguizmo::imguizmo
            iconfontcppheaders::iconfontcppheaders
)

# background image: open-file dialog
if(WIN32)
    target_link_libraries(engine_editor PRIVATE ole32 shell32 uuid)
endif()

target_sources(engine_editor PRIVATE
    src/editor/AssetImportManager.cpp
    src/editor/BackgroundImage.cpp
    src/editor/ContentBrowserWatcher.cpp
    src/editor/CreateGameProject.cpp
    src/editor/Editor.cpp
    src/editor/EditorCamera.cpp
    src/editor/EditorModule.cpp
    src/editor/EditorPreferences.cpp
    src/editor/EditorWidgets.cpp
    src/editor/EntityBaseline.cpp
    src/editor/PrefabAuthoringAdapter.cpp
    src/editor/PrefabPlayMarks.cpp
    src/editor/PrefabWorkspace.cpp
    src/editor/ViewPie.cpp
    src/editor/history/ActiveCameraEdit.cpp
    src/editor/history/AuthoredCommand.cpp
    src/editor/history/AuthoredEdit.cpp
    src/editor/history/CommandGroup.cpp
    src/editor/history/ComponentCommands.cpp
    src/editor/history/DocumentNodeCommands.cpp
    src/editor/history/EntityCommands.cpp
    src/editor/history/EntityIds.cpp
    src/editor/history/History.cpp
    src/editor/history/SubtreeRecord.cpp
    src/editor/IdeSettings.cpp
    src/editor/panels/HierarchyPanel.cpp
    src/editor/panels/AssetImportPanel.cpp
    src/editor/panels/ContentBrowserPanel.cpp
    src/editor/panels/InputSettingsPanel.cpp
    src/editor/panels/InspectorPanel.cpp
    src/editor/panels/LogPanel.cpp
    src/editor/panels/PostProcessingPanel.cpp
    src/editor/panels/PreferencesPanel.cpp
    src/editor/panels/ProfilerPanel.cpp
    src/editor/panels/ProjectSettingsPanel.cpp
    src/editor/WorldSceneOps.cpp
)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
