engine_add_module(physics_script)

target_sources(engine_physics_script PRIVATE
    src/physics_script/PhysicsBindings.cpp
    src/physics_script/PhysicsScriptModule.cpp
)

# PUBLIC script_bindings: PhysicsBindings.h names sol::state
target_link_libraries(engine_physics_script
    PUBLIC mir::script
           mir::script_bindings
    PRIVATE mir::core
            mir::physics
)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
