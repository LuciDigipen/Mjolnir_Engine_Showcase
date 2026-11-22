engine_add_module_tests(gameui_imgui
    ImGuiUIBackend.tests.cpp
)

# a headless ImGui context, and the cooked textures the tests write
target_link_libraries(engine_gameui_imgui_tests PRIVATE imgui::imgui mir::assets mir::input)

# a real TrueType font for the font tests
target_compile_definitions(engine_gameui_imgui_tests PRIVATE MIR_TEST_FONT="${CMAKE_SOURCE_DIR}/fonts/Inter.ttf")
