engine_add_module_tests(terrain_runtime TerrainJobExecutor.tests.cpp)
target_link_libraries(engine_terrain_runtime_tests PRIVATE mir::jobs)
target_include_directories(engine_terrain_runtime_tests PRIVATE ../../terrain/tests)
