engine_add_module_tests(gameui_imgui
    ImGuiUIBackend.tests.cpp
)

# a headless ImGui context, and the cooked textures the tests write
target_link_libraries(engine_gameui_imgui_tests PRIVATE imgui::imgui mir::assets mir::input)
