engine_add_module_tests(gameui_script
    UIBindings.tests.cpp
)

target_link_libraries(engine_gameui_script_tests PRIVATE mir::core mir::input)
