engine_add_module_tests(editor
    ComponentCommands.tests.cpp
    EntityCommands.tests.cpp
    History.tests.cpp
)

# editor links core privately, but its headers include core's; scene stands in for App's scene ops
target_link_libraries(engine_editor_tests PRIVATE mir::core mir::scene)
