engine_add_module_tests(flight_script
    FlightBindings.tests.cpp
)

# flight_script links flight privately; the fixture builds a FlightModule and an aircraft config
target_link_libraries(engine_flight_script_tests PRIVATE mir::core mir::physics mir::flight mir::assets)
