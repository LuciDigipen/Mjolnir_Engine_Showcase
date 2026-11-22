engine_add_module(gameui_script)

target_sources(engine_gameui_script PRIVATE
    src/gameui_script/GameUIScriptModule.cpp
    src/gameui_script/UIBindings.cpp
)

# PUBLIC script_bindings: UIBindings.h names sol::state
target_link_libraries(engine_gameui_script
    PUBLIC mir::gameui
           mir::script
           mir::script_bindings
    PRIVATE mir::assets
            mir::core
)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
