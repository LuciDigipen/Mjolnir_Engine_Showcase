engine_add_module(gameui_imgui)

target_sources(engine_gameui_imgui PRIVATE
    src/gameui_imgui/GameUIImGuiModule.cpp
    src/gameui_imgui/ImGuiUIBackend.cpp
)

# A bridge: gameui names no graphics API, and imguiruntime knows no game UI.
# PUBLIC gameui: ImGuiUIBackend.h implements its interface. imgui stays private:
# no ImGui type appears in a public header
target_link_libraries(engine_gameui_imgui
    PUBLIC mir::gameui
    PRIVATE mir::core
            mir::imguiruntime
            mir::window
            imgui::imgui
)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
