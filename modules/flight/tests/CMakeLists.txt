engine_add_module_tests(flight
    Aerodynamics.tests.cpp
    AerodynamicsL2.tests.cpp
    AerodynamicsL3.tests.cpp
    AircraftConfig.tests.cpp
    AircraftFields.tests.cpp
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
    ShippedAircraft.tests.cpp
    Telemetry.tests.cpp
)

target_compile_definitions(engine_flight_tests PRIVATE MIR_GAMES_DIR="${CMAKE_SOURCE_DIR}/games")

# flight links renderer and camera privately; the telemetry test adds a Camera and reads the cut list, and the
# camera test registers the chase controller component. The field test reads a component through scene's Json codec.
find_package(nlohmann_json CONFIG REQUIRED)
target_link_libraries(engine_flight_tests PRIVATE mir::renderer mir::camera mir::scene nlohmann_json::nlohmann_json)
