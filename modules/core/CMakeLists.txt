
# Two libraries from one folder, split by include root.
# core_base (include/): log, fs, math, platform. Process-wide, set up in main, and
# links only third-party code. core (include_ecs/): the ECS, built on core_base. A
# library linking only core_base cannot find an ECS header
engine_add_module(core_base)

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/include/core/EngineVersion.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated_include/core/EngineVersion.h
    @ONLY)
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/include/core/WorldConfig.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated_include/core/WorldConfig.h
    @ONLY)
target_include_directories(engine_core_base PUBLIC ${CMAKE_CURRENT_BINARY_DIR}/generated_include)

target_sources(engine_core_base PRIVATE
    src/core/log/Log.cpp
    src/core/log/LogHistory.cpp
    src/core/fs/Paths.cpp
    src/core/fs/FileIO.cpp
    src/core/platform/Process.cpp
    src/core/platform/CrashHandler.cpp)

# get external libraries here
find_package(spdlog CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)

# spdlog stays private
target_link_libraries(engine_core_base PRIVATE spdlog::spdlog)

# glm is PUBLIC: glm types are in the public math and component headers
target_link_libraries(engine_core_base PUBLIC glm::glm)

# UserDataDir: SHGetKnownFolderPath and CoTaskMemFree
if(WIN32)
    target_link_libraries(engine_core_base PRIVATE shell32 ole32)
endif()

engine_add_module(core INCLUDE_DIR include_ecs)

target_sources(engine_core PRIVATE
    src/core/ecs/TransformHierarchy.cpp
    src/core/ecs/HierarchyIndex.cpp
    src/core/ecs/ComponentRegistry.cpp
    src/core/ecs/EntityPresetRegistry.cpp
    src/core/ecs/GroundQuery.cpp
    src/core/ecs/RuntimeQuery.cpp
    src/core/ecs/DeferredAccess.cpp
    src/core/ecs/CoreModule.cpp)

# PUBLIC: ECS headers include log, math and fs headers
target_link_libraries(engine_core PUBLIC mir::core_base)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
