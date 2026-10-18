engine_add_module_tests(camera
    CameraController.tests.cpp
)

# Test-only: drives free-fly through a real InputState.
target_link_libraries(engine_camera_tests PRIVATE mir::input)
