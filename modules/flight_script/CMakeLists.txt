engine_add_module(flight_script)

target_sources(engine_flight_script PRIVATE
    src/flight_script/FlightBindings.cpp
    src/flight_script/FlightScriptModule.cpp
)

# PUBLIC script_bindings: FlightBindings.h names sol::state
target_link_libraries(engine_flight_script
    PUBLIC mir::script
           mir::script_bindings
    PRIVATE mir::core
            mir::flight
            mir::physics
)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
