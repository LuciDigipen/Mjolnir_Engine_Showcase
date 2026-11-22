engine_add_module(actions)

target_sources(engine_actions PRIVATE
    src/actions/ActionScheduler.cpp
    src/actions/ActionSystem.cpp
    src/actions/ActionsModule.cpp
    src/actions/Ease.cpp
)

# PUBLIC core: Entity, WorldVec3, ModuleContext and ISystem appear in include/actions/
target_link_libraries(engine_actions
    PUBLIC mir::core
)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
