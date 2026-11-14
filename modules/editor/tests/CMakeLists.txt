find_package(nlohmann_json CONFIG REQUIRED)

engine_add_module_tests(editor
    ActiveCameraEdit.tests.cpp
    Authoring.tests.cpp
    AuthoredEdit.tests.cpp
    BackgroundImage.tests.cpp
    BuildJob.tests.cpp
    BuildSettings.tests.cpp
    ComponentCommands.tests.cpp
    ContentBrowserWatcher.tests.cpp
    ContentThumbnails.tests.cpp
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
    SettingsPanels.tests.cpp
    SnapToTerrain.tests.cpp
    ViewPie.tests.cpp
    WorldSceneOps.tests.cpp
    ShaderHotReload.tests.cpp
)

# editor links scene privately; the tests build scenes for the scene ops and prefab previews
target_link_libraries(engine_editor_tests PRIVATE mir::jobs mir::scene mir::particles imgui::imgui nlohmann_json::nlohmann_json)
file(TO_CMAKE_PATH "${SLANGC_EXECUTABLE}" MIR_EDITOR_SLANGC_PATH)
target_compile_definitions(engine_editor_tests PRIVATE MIR_EDITOR_SLANGC="${MIR_EDITOR_SLANGC_PATH}")

file(TO_CMAKE_PATH "${CMAKE_BINARY_DIR}/shader_build/shader_manifest.json" MIR_EDITOR_MANIFEST_PATH)
target_compile_definitions(engine_editor_tests PRIVATE MIR_EDITOR_SHADER_MANIFEST="${MIR_EDITOR_MANIFEST_PATH}")

# BuildJob.tests run the real MjolnirBuild, and a probe that stops where a real build is too quick to cancel
add_executable(engine_editor_build_probe BuildProbe.cpp)
engine_target_utf8(engine_editor_build_probe)
add_dependencies(engine_editor_tests MjolnirBuild engine_editor_build_probe)
target_compile_definitions(engine_editor_tests PRIVATE MIR_EDITOR_BUILD_TOOL="$<TARGET_FILE:MjolnirBuild>"
    MIR_EDITOR_BUILD_PROBE="$<TARGET_FILE:engine_editor_build_probe>")
