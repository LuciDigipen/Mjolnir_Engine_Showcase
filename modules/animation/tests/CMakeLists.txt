engine_add_module_tests(animation ModelAnimation.tests.cpp ModelPreparation.tests.cpp)
# the renderer's fake hosts and cooked-directory helpers
target_include_directories(engine_animation_tests PRIVATE ${CMAKE_SOURCE_DIR}/modules/renderer/tests)
target_link_libraries(engine_animation_tests PRIVATE mir::jobs)
