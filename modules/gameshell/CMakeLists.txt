engine_add_module(gameshell)

# PUBLIC: UIContext (gameui) appears in include/gameshell/. Nothing else: the
# screens are built on gameui's public API alone, as a game's would be.
target_link_libraries(engine_gameshell PUBLIC mir::gameui PRIVATE mir::assets mir::core mir::serial)

target_sources(engine_gameshell PRIVATE
    src/gameshell/GameHud.cpp
    src/gameshell/GameShell.cpp
    src/gameshell/GameShellModule.cpp
    src/gameshell/PauseMenu.cpp
    src/gameshell/UIStyleFile.cpp
)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
