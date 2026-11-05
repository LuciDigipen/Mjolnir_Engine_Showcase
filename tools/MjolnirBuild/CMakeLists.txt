find_package(nlohmann_json CONFIG REQUIRED)

# Packages one game with a game build template; the engine never links it.
add_library(mjolnir_build STATIC
    BuildOutput.cpp
    CompileLog.cpp
    GameBuild.cpp
    GameBuildCompile.cpp
    VisualStudio.cpp
)
target_include_directories(mjolnir_build PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mjolnir_build
    PRIVATE mir::assets mir::core mir::project mir::serial mir::terrain nlohmann_json::nlohmann_json)

add_executable(MjolnirBuild main.cpp)
engine_target_utf8(MjolnirBuild)
target_link_libraries(MjolnirBuild PRIVATE mjolnir_build mir::core)
target_compile_definitions(MjolnirBuild PRIVATE
    MJOLNIR_BUILD_ASSET_COOKER="$<TARGET_FILE:AssetCooker>"
    MJOLNIR_BUILD_TERRAIN_TOOL="$<TARGET_FILE:TerrainTool>"
    # the engine built beside it, which a template is compared with
    MJOLNIR_BUILD_EDITOR_INFO="$<TARGET_FILE_DIR:MjolnirEngine>/game_build_info.json")
add_dependencies(MjolnirBuild AssetCooker TerrainTool)

if(BUILD_TESTING)
    # cmake.exe in a folder of its own, which the game build step tests put first on PATH
    add_executable(mjolnir_build_fake_cmake tests/FakeCMake.cpp)
    set_target_properties(mjolnir_build_fake_cmake PROPERTIES
        OUTPUT_NAME cmake
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fake_cmake")

    add_executable(mjolnir_build_tests
        tests/BuildOutput.tests.cpp
        tests/CompileLog.tests.cpp
        tests/GameBuild.tests.cpp
        tests/GameBuildCompile.tests.cpp
        tests/VisualStudio.tests.cpp
    )
    engine_target_utf8(mjolnir_build_tests)
    target_link_libraries(mjolnir_build_tests PRIVATE
        mjolnir_build mir::assets mir::core mir::project nlohmann_json::nlohmann_json Catch2::Catch2WithMain)
    target_compile_definitions(mjolnir_build_tests PRIVATE
        MJOLNIR_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
        MJOLNIR_ENGINE_COOK_DIR="${CMAKE_BINARY_DIR}/cooked_assets"
        MJOLNIR_BUILD_ASSET_COOKER="$<TARGET_FILE:AssetCooker>"
        MJOLNIR_BUILD_TERRAIN_TOOL="$<TARGET_FILE:TerrainTool>"
        MJOLNIR_BUILD_FAKE_CMAKE_DIR="$<TARGET_FILE_DIR:mjolnir_build_fake_cmake>")
    add_dependencies(mjolnir_build_tests AssetCooker TerrainTool mjolnir_build_fake_cmake)
    catch_discover_tests(mjolnir_build_tests)
endif()
