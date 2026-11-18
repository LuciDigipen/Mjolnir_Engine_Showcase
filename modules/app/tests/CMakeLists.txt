add_executable(engine_app_tests AppRuntime.tests.cpp)
engine_target_utf8(engine_app_tests)
target_link_libraries(engine_app_tests PRIVATE mir::app Catch2::Catch2WithMain)
target_compile_definitions(engine_app_tests PRIVATE MIR_APP_TEST_ROOT="${CMAKE_SOURCE_DIR}")
set_target_properties(engine_app_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
catch_discover_tests(engine_app_tests PROPERTIES LABELS smoke RUN_SERIAL TRUE)

# Keep process-wide registrations valid across the suite's repeated App lifetimes.
add_test(NAME engine_app_tests.single-process COMMAND engine_app_tests)
set_tests_properties(engine_app_tests.single-process PROPERTIES LABELS smoke RUN_SERIAL TRUE)
