engine_add_module(gameui)

# PUBLIC: AssetId (assets) and InputState (input) appear in include/gameui/.
# No graphics API: what draws the UI is a backend bridge (gameui_imgui).
target_link_libraries(engine_gameui
    PUBLIC mir::assets
           mir::input
           mir::core
)

target_sources(engine_gameui PRIVATE
    src/gameui/GameUIModule.cpp
    src/gameui/UIContext.cpp
    src/gameui/UIDemoScreen.cpp
    src/gameui/UIDrawList.cpp
    src/gameui/UIFonts.cpp
    src/gameui/UINavInput.cpp
    src/gameui/UIState.cpp
    src/gameui/UIStyleFields.cpp
    src/gameui/UITextures.cpp
    src/gameui/UITransform.cpp
    src/gameui/UIWidgets.cpp
)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
