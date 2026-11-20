engine_add_module(actions_script)

target_sources(engine_actions_script PRIVATE
    src/actions_script/ActionBindings.cpp
    src/actions_script/ActionsScriptModule.cpp
)

# PUBLIC script_bindings: ActionBindings.h names sol::state
target_link_libraries(engine_actions_script
    PUBLIC mir::script
           mir::script_bindings
    PRIVATE mir::actions
            mir::core
)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
