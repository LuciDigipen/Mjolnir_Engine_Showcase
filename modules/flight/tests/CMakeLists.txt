engine_add_module_tests(flight
    Aerodynamics.tests.cpp
    AerodynamicsL2.tests.cpp
    AircraftConfig.tests.cpp
    AirData.tests.cpp
    FlightActuators.tests.cpp
    FlightFrames.tests.cpp
    FlightInput.tests.cpp
    FlightStepGate.tests.cpp
    LandingGear.tests.cpp
    Propulsion.tests.cpp
    Telemetry.tests.cpp
)

# flight links renderer privately for camera cuts; the telemetry test adds a Camera and reads the cut list.
target_link_libraries(engine_flight_tests PRIVATE mir::renderer)
