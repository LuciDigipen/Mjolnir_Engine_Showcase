find_package(nlohmann_json CONFIG REQUIRED)

engine_add_module_tests(editor
    ActiveCameraEdit.tests.cpp
    Authoring.tests.cpp
    AuthoredEdit.tests.cpp
    BackgroundImage.tests.cpp
    ComponentCommands.tests.cpp
    ContentBrowserWatcher.tests.cpp
    DocumentNodeCommands.tests.cpp
    EditorCamera.tests.cpp
    EntityCommands.tests.cpp
    EntityDragPayload.tests.cpp
    History.tests.cpp
    MaterialDraft.tests.cpp
    MaterialRecipe.tests.cpp
    PrefabAuthoringAdapter.tests.cpp
    PrefabPlayMarks.tests.cpp
    PrefabWorkspace.tests.cpp
    SnapToTerrain.tests.cpp
    ViewPie.tests.cpp
)

# editor links scene privately; the tests build scenes for the scene ops and prefab previews
target_link_libraries(engine_editor_tests PRIVATE mir::scene mir::particles imgui::imgui nlohmann_json::nlohmann_json)
