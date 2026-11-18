engine_add_module(camera PCH_HEADERS glm/glm.hpp)

target_link_libraries(engine_camera
    PUBLIC mir::core
    PRIVATE mir::input
)

target_sources(engine_camera PRIVATE
    src/camera/CameraModule.cpp
    src/camera/CameraControllerSystem.cpp
    src/camera/ComponentRegistration.cpp
)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
