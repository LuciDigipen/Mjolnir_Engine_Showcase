engine_add_module(animation PCH_HEADERS glm/glm.hpp)
target_sources(engine_animation PRIVATE
    src/animation/AnimationModule.cpp
    src/animation/ModelAnimation.cpp
    src/animation/ModelEvaluation.cpp
    src/animation/ModelLibrary.cpp
    src/animation/ModelPreparation.cpp
    src/animation/ModelSystem.cpp
)
target_link_libraries(engine_animation PUBLIC mir::assets mir::core mir::renderer)
target_link_libraries(engine_animation PRIVATE mir::jobs)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
