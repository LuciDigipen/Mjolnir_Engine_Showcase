engine_add_module(flight)

target_link_libraries(engine_flight
    PUBLIC mir::core
           mir::physics
           mir::input
           mir::assets
    PRIVATE mir::camera
            mir::renderer
)

find_package(nlohmann_json CONFIG REQUIRED)
target_link_libraries(engine_flight PRIVATE nlohmann_json::nlohmann_json)

target_sources(engine_flight PRIVATE
    src/flight/AerodynamicModel.cpp
    src/flight/AircraftConfig.cpp
    src/flight/AircraftConfigStore.cpp
    src/flight/AircraftConfigSystem.cpp
    src/flight/AirData.cpp
    src/flight/ComponentRegistration.cpp
    src/flight/FlightActuators.cpp
    src/flight/FlightCameraSystem.cpp
    src/flight/FlightCurve.cpp
    src/flight/FlightEnvironment.cpp
    src/flight/FlightFrames.cpp
    src/flight/FlightInput.cpp
    src/flight/FlightInputSystem.cpp
    src/flight/FlightLandingGear.cpp
    src/flight/FlightModule.cpp
    src/flight/FlightPropulsion.cpp
    src/flight/FlightReset.cpp
    src/flight/FlightStepGate.cpp
    src/flight/FlightTelemetry.cpp
    src/flight/FlightTouchdownSystem.cpp
)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
