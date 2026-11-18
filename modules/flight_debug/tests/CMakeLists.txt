engine_add_module_tests(flight_debug
    FlightDebug.tests.cpp
    FlightDebugTuning.tests.cpp
)

# flight_debug links flight privately; the tests build the row text from flight types.
target_link_libraries(engine_flight_debug_tests PRIVATE mir::flight)
