add_executable(FlightEffectsCheck main.cpp)
find_package(nlohmann_json CONFIG REQUIRED)
target_link_libraries(FlightEffectsCheck PRIVATE mir::particles mir::particles_gpu mir::particles_runtime
    mir::particles_script mir::script_bindings mir::assets mir::window nlohmann_json::nlohmann_json)
set_target_properties(FlightEffectsCheck PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_dependencies(FlightEffectsCheck MjolnirEngine)
set(flight_origin 0)
if(MIR_LARGE_WORLD)
    set(flight_origin 1000000)
endif()
add_test(NAME flight-effects.cpu COMMAND FlightEffectsCheck
    "${CMAKE_BINARY_DIR}/flight-effects-cpu.json" cpu on 8 ${flight_origin} profile validation)
add_test(NAME smoke.flight-effects.gpu COMMAND FlightEffectsCheck
    "${CMAKE_BINARY_DIR}/flight-effects-smoke.json" raster on 8 0 smoke validation)
set_tests_properties(flight-effects.cpu PROPERTIES TIMEOUT 240)
set_tests_properties(smoke.flight-effects.gpu PROPERTIES LABELS smoke RUN_SERIAL TRUE TIMEOUT 180 SKIP_RETURN_CODE 77)
