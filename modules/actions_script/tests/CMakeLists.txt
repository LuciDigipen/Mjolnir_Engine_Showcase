engine_add_module_tests(actions_script
    ActionBindings.tests.cpp
)

target_link_libraries(engine_actions_script_tests PRIVATE mir::actions mir::core)
