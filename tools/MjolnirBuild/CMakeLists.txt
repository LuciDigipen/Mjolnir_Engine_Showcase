find_package(nlohmann_json CONFIG REQUIRED)

# Packages one game with a player template; the engine never links it.
add_library(mjolnir_build STATIC
    BuildOutput.cpp
    GameBuild.cpp
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
    MJOLNIR_BUILD_EDITOR_INFO="$<TARGET_FILE_DIR:MjolnirEngine>/player_info.json")
add_dependencies(MjolnirBuild AssetCooker TerrainTool)

if(BUILD_TESTING)
    add_executable(mjolnir_build_tests
        tests/BuildOutput.tests.cpp
        tests/GameBuild.tests.cpp
    )
    engine_target_utf8(mjolnir_build_tests)
    target_link_libraries(mjolnir_build_tests PRIVATE
        mjolnir_build mir::assets mir::core nlohmann_json::nlohmann_json Catch2::Catch2WithMain)
    target_compile_definitions(mjolnir_build_tests PRIVATE
        MJOLNIR_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
        MJOLNIR_ENGINE_COOK_DIR="${CMAKE_BINARY_DIR}/cooked_assets"
        MJOLNIR_BUILD_ASSET_COOKER="$<TARGET_FILE:AssetCooker>"
        MJOLNIR_BUILD_TERRAIN_TOOL="$<TARGET_FILE:TerrainTool>")
    add_dependencies(mjolnir_build_tests AssetCooker TerrainTool)
    catch_discover_tests(mjolnir_build_tests)
endif()
