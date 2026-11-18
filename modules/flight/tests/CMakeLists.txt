engine_add_module_tests(flight
    Aerodynamics.tests.cpp
    AerodynamicsL2.tests.cpp
    AircraftConfig.tests.cpp
    AirData.tests.cpp
    FlightActuators.tests.cpp
    FlightCamera.tests.cpp
    FlightFrames.tests.cpp
    FlightInput.tests.cpp
    FlightStepGate.tests.cpp
    HotReload.tests.cpp
    LandingGear.tests.cpp
    LiveTuning.tests.cpp
    Propulsion.tests.cpp
    Telemetry.tests.cpp
)

# flight links renderer and camera privately; the telemetry test adds a Camera and reads the cut list, and the
# camera test registers the chase controller component.
target_link_libraries(engine_flight_tests PRIVATE mir::renderer mir::camera)
