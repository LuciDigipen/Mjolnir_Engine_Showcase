cmake_minimum_required(VERSION 3.26)


option(MIR_LARGE_WORLD "Double-precision world positions and Jolt" ON)
if(MIR_LARGE_WORLD)
    list(APPEND VCPKG_MANIFEST_FEATURES large-world)
endif()

project(Mjolnir
    VERSION 0.1.0
    DESCRIPTION "A modular 3D engine"
    # C is needed for volk.c
    LANGUAGES CXX C)

# C++ 20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set (CMAKE_CXX_EXTENSIONS OFF)

# Editing the manifest must re-run configure, otherwise vcpkg never installs
# the new dependency and IntelliSense keeps the stale include paths.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg.json")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(cmake/EnginePlatform.cmake)
include(cmake/EngineModule.cmake)
include(cmake/Shaders.cmake)
include(cmake/Assets.cmake)
include(cmake/Fonts.cmake)
include(cmake/Branding.cmake)
include(cmake/Player.cmake)

option(BUILD_TESTING "Build unit tests (per module, run via ctest)" ON)

# OFF builds MjolnirEngine as the game alone: editor, editortheme and ImGuizmo
# are not built, and nothing links engine_editor. The windows-msvc-game-*
# presets set it.
option(MJOLNIR_WITH_EDITOR "Build the editor into MjolnirEngine" ON)

option(MIR_WITH_AUDIO "Build Wwise audio and Lua audio bindings" ON)

# OFF (default): no network fetch, no NVIDIA SDK dependency - DLSS Ray
# Reconstruction anti-aliasing stays unreachable, same as unsupported hardware.
# ON: fetches the public NVIDIA/DLSS SDK via CMake FetchContent at configure
# time (cmake/FindNGX.cmake), pinned to an exact release tag - never committed.
option(MIR_WITH_DLSS "Fetch NGX/DLSS-RR SDK and build Ray Reconstruction" OFF)

# Optional DDGI SDK and offline-compiled probe update shaders. OFF does not
# fetch RTXGI or require DXC; runtime GI is integrated separately.
option(MIR_WITH_RTXGI "Fetch RTXGI DDGI SDK and build Vulkan probe shaders" OFF)
if(BUILD_TESTING)
    enable_testing()
    find_package(Catch2 3 CONFIG REQUIRED)
    include(CTest)
    include(Catch)
endif()

add_subdirectory(third_party)
add_subdirectory(modules/animation)
add_subdirectory(modules/app)
add_subdirectory(modules/assets)
if(MIR_WITH_AUDIO)
    add_subdirectory(modules/audio)
    add_subdirectory(modules/audio_script)
endif()
add_subdirectory(modules/camera)
add_subdirectory(modules/core)
add_subdirectory(modules/editor)
add_subdirectory(modules/editortheme)
add_subdirectory(modules/gameshell)
add_subdirectory(modules/gameui)
add_subdirectory(modules/gameui_imgui)
add_subdirectory(modules/imguiruntime)
add_subdirectory(modules/input)
add_subdirectory(modules/input_script)
add_subdirectory(modules/jobs)
add_subdirectory(modules/particles)
add_subdirectory(modules/particles_gpu)
add_subdirectory(modules/particles_runtime)
add_subdirectory(modules/particles_script)
add_subdirectory(modules/physics)
add_subdirectory(modules/physics_layers)
add_subdirectory(modules/physics_script)
add_subdirectory(modules/prefab)
add_subdirectory(modules/project)
add_subdirectory(modules/renderer)
add_subdirectory(modules/renderer_script)
add_subdirectory(modules/scene)
add_subdirectory(modules/script)
add_subdirectory(modules/serial)
add_subdirectory(modules/terrain)
add_subdirectory(modules/terrain_runtime)
add_subdirectory(modules/terrain_script)
add_subdirectory(modules/window)

add_subdirectory(tools/AssetCooker)
add_subdirectory(tools/ShaderPublisher)
add_subdirectory(tools/ErrorReporter)
add_subdirectory(tools/TerrainTool)
add_subdirectory(tools/MjolnirBuild)

add_executable(MjolnirEngine WIN32)
target_sources(MjolnirEngine PRIVATE main.cpp)
engine_target_utf8(MjolnirEngine)
if(ENGINE_PLATFORM STREQUAL "Windows")
    target_sources(MjolnirEngine PRIVATE MjolnirEngine.rc)
    set_source_files_properties(MjolnirEngine.rc PROPERTIES
        INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}"
        OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/branding/mjolnir.ico")
endif()

foreach(surface_shader default model unlit error sky)
    engine_add_shaders(MjolnirEngine
        SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders/${surface_shader}Surface.slang"
        ENTRIES vertexMain:vertex fragmentMain:fragment)
endforeach()

if(MIR_WITH_RTXGI)
    engine_add_shaders(MjolnirEngine
        SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders/raytraceGi.slang
        INCLUDE_DIRS ${MIR_RTXGI_INCLUDE_DIR}
        DEFINES HLSL=1 RTXGI_COORDINATE_SYSTEM=2
        EXTRA_DEPENDS ${MIR_RTXGI_SHADER_DEPENDS}
        ENTRIES rayGenMain:raygeneration missMain:miss closestHitMain:closesthit
            anyHitMain:anyhit shadowMissMain:miss shadowAnyHitMain:anyhit)
    engine_add_shaders(MjolnirEngine
        SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders/ddgiProbe.slang
        INCLUDE_DIRS ${MIR_RTXGI_INCLUDE_DIR}
        DEFINES HLSL=1 RTXGI_COORDINATE_SYSTEM=2
        EXTRA_DEPENDS ${MIR_RTXGI_SHADER_DEPENDS}
        ENTRIES probeRayGen:raygeneration probeMiss:miss probeClosestHit:closesthit
            probeAnyHit:anyhit shadowMissMain:miss shadowAnyHitMain:anyhit)
    get_property(rtxgi_shader_outputs GLOBAL PROPERTY MIR_RTXGI_SHADER_OUTPUTS)
    add_custom_command(TARGET MjolnirEngine POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:MjolnirEngine>/shaders/rtxgi"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            ${rtxgi_shader_outputs} "$<TARGET_FILE_DIR:MjolnirEngine>/shaders/rtxgi"
        VERBATIM COMMAND_EXPAND_LISTS)
endif()

# ReportFatal looks for the reporter beside the engine, so every build ships it there
add_dependencies(MjolnirEngine MjolnirErrorReporter)
add_custom_command(TARGET MjolnirEngine POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:MjolnirErrorReporter>" "$<TARGET_FILE_DIR:MjolnirEngine>"
    VERBATIM)

# Local development binding used by the editor's one-shot import workflow.
# It is generated into the build tree and never written into game data.
if(MJOLNIR_WITH_EDITOR)
    file(TO_CMAKE_PATH "${CMAKE_SOURCE_DIR}" asset_workspace_source_root)
    file(GENERATE
        OUTPUT "$<TARGET_FILE_DIR:MjolnirEngine>/asset_workspace.json"
        CONTENT "{\n  \"version\": 1,\n  \"sourceRoot\": \"${asset_workspace_source_root}\",\n  \"assetCooker\": \"$<TARGET_FILE:AssetCooker>\",\n  \"buildTool\": \"$<TARGET_FILE:MjolnirBuild>\"\n}\n")
    # File > Build runs it
    add_dependencies(MjolnirEngine MjolnirBuild)
endif()

if(MSVC)
    set_target_properties(MjolnirEngine PROPERTIES LINK_FLAGS "/ENTRY:mainCRTStartup")
endif()

engine_add_shaders(MjolnirEngine
    SOURCE assets/shaders/SsgiDepth.slang
    ENTRIES initMain:compute reduceMain:compute)

engine_add_shaders(MjolnirEngine
    SOURCE assets/shaders/SsgiTrace.slang
    ENTRIES traceMain:compute)

engine_add_shaders(MjolnirEngine
    SOURCE assets/shaders/SsgiFilter.slang
    ENTRIES temporalMain:compute spatialMain:compute resolveMain:compute)

engine_add_shaders(MjolnirEngine
    SOURCE assets/shaders/SsgiComposite.slang
    ENTRIES vertexMain:vertex fragmentMain:fragment)

engine_add_shaders(MjolnirEngine
    SOURCE assets/shaders/default.slang
    ENTRIES vertexMain:vertex fragmentMain:fragment)

engine_add_shaders(MjolnirEngine
    SOURCE assets/shaders/unlit.slang
    ENTRIES vertexMain:vertex fragmentMain:fragment)

engine_add_shaders(MjolnirEngine
    SOURCE assets/shaders/error.slang
    ENTRIES vertexMain:vertex fragmentMain:fragment)

engine_add_shaders(MjolnirEngine
    SOURCE assets/shaders/model.slang
    ENTRIES vertexMain:vertex fragmentMain:fragment)

engine_add_shaders(MjolnirEngine
    SOURCE assets/shaders/sky.slang
    ENTRIES vertexMain:vertex fragmentMain:fragment)

engine_add_shaders(MjolnirEngine
    SOURCE assets/shaders/tonemap.slang
    ENTRIES vertexMain:vertex fragmentMain:fragment)

engine_add_shaders(MjolnirEngine
    SOURCE assets/shaders/fxaa.slang
    ENTRIES vertexMain:vertex fragmentMain:fragment)

engine_add_shaders(MjolnirEngine
    SOURCE assets/shaders/bloom.slang
    ENTRIES vertexMain:vertex brightpassMain:fragment downsampleMain:fragment upsampleMain:fragment)

engine_add_shaders(MjolnirEngine
    SOURCE assets/shaders/depthOfField.slang
    ENTRIES vertexMain:vertex cocMain:fragment nearBlurMain:fragment farBlurMain:fragment compositeMain:fragment)

engine_add_shaders(MjolnirEngine
    SOURCE assets/shaders/velocity.slang
    ENTRIES vertexMain:vertex fragmentMain:fragment)

engine_add_shaders(MjolnirEngine
    SOURCE assets/shaders/motionBlur.slang
    ENTRIES vertexMain:vertex compositeMain:fragment)

engine_add_shaders(MjolnirEngine
    SOURCE modules/particles_gpu/shaders/particlePool.slang
    ENTRIES vertexMain:vertex fragmentMain:fragment ribbonVertex:vertex ribbonFragment:fragment)
# Incremental build trees must not package the replaced fixed compute shader.
file(REMOVE "${CMAKE_BINARY_DIR}/shaders/particlePool.computeMain.spv")
engine_add_shaders(MjolnirEngine
    SOURCE modules/particles_gpu/shaders/particleViewSort.slang
    ENTRIES computeMain:compute)

engine_add_shaders(MjolnirEngine
    SOURCE assets/shaders/fxComposition.slang
    ENTRIES vertexMain:vertex fragmentMain:fragment)

if(BUILD_TESTING)
    add_subdirectory(tools/ParticleGpuCheck)
    add_subdirectory(tools/TextureGpuCheck)
    add_subdirectory(tools/ModelWorldsGpuCheck)
endif()

engine_add_shaders(MjolnirEngine
    SOURCE assets/shaders/shadowDepth.slang
    ENTRIES vertexMain:vertex)

engine_add_shaders(MjolnirEngine
    SOURCE assets/shaders/debugWireframe.slang
    ENTRIES vertexMain:vertex fragmentMain:fragment)

engine_add_shaders(MjolnirEngine
    SOURCE assets/shaders/debugLines.slang
    ENTRIES vertexMain:vertex fragmentMain:fragment)

engine_add_shaders(MjolnirEngine
    SOURCE assets/shaders/shadowDistance.slang
    ENTRIES vertexMain:vertex fragmentMain:fragment)

engine_add_shaders(MjolnirEngine
    SOURCE assets/shaders/raytrace.slang
    ENTRIES rayGenMain:raygeneration missMain:miss closestHitMain:closesthit shadowMissMain:miss
            anyHitMain:anyhit shadowAnyHitMain:anyhit)

include(cmake/GameContent.cmake)

# Every games/*/ with a game.json is a game; its assetsRoot gets cooked
file(GLOB game_manifests CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/games/*/game.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${game_manifests})

set(game_names "")
set(game_asset_roots "")
set(game_copy_commands "")
set(terrain_sync_commands "")
set(terrain_package_commands "")
foreach(manifest IN LISTS game_manifests)
    get_filename_component(game_dir "${manifest}" DIRECTORY)
    get_filename_component(game_name "${game_dir}" NAME)

    file(READ "${manifest}" game_json)

    # A game whose large files were left out of the clone is not a game on this machine.
    game_content_present("${game_dir}" "${game_json}" game_has_content)
    if(NOT game_has_content)
        message(STATUS "Skipping game ${game_name}: its LFS content is not here; see games/${game_name}/README.md")
        continue()
    endif()
    list(APPEND game_names "${game_name}")

    string(JSON game_json_length ERROR_VARIABLE game_json_error LENGTH "${game_json}")
    if(game_json_error)
        message(WARNING "${manifest} is not valid JSON (${game_json_error}), cooking its default assets root")
    endif()

    # default must match LoadGameProject in app/GameProject.cpp
    string(JSON assets_root ERROR_VARIABLE assets_root_error GET "${game_json}" assetsRoot)
    if(assets_root_error)
        set(assets_root "assets")
    endif()

    # The id root LoadGameProject builds: "games/<dir>/" + assetsRoot normalized on
    # its own, so a root it refuses (outside the game, ".", absolute) is not cooked.
    cmake_path(SET assets_norm NORMALIZE "${assets_root}")
    string(REGEX REPLACE "/+$" "" assets_norm "${assets_norm}")
    if(assets_norm STREQUAL "" OR assets_norm STREQUAL "." OR assets_norm MATCHES "^\\.\\.(/|$)"
       OR IS_ABSOLUTE "${assets_root}" OR assets_root MATCHES "^[/\\\\]|:")
        message(WARNING "${manifest}: assetsRoot \"${assets_root}\" is not a relative path inside the game; not cooked")
    else()
        set(assets_rel "games/${game_name}/${assets_norm}")
        # skip missing roots (AssetCooker fails on them)
        if(IS_DIRECTORY "${CMAKE_SOURCE_DIR}/${assets_rel}")
            list(APPEND game_asset_roots "${assets_rel}")
            list(APPEND game_copy_commands COMMAND "${CMAKE_COMMAND}" -E copy_directory_if_different
                 "${CMAKE_SOURCE_DIR}/${assets_rel}" "$<TARGET_FILE_DIR:MjolnirEngine>/${assets_rel}")

            # A game build can only read cooked Prefabs, never the source tree.
            if(NOT MJOLNIR_WITH_EDITOR)
                list(APPEND game_copy_commands COMMAND "${CMAKE_COMMAND}" -E rm -rf
                     "$<TARGET_FILE_DIR:MjolnirEngine>/${assets_rel}/prefabs")
            endif()

            # Releases live outside assetsRoot, so they are never cooked or copied with the assets.
            file(GLOB game_terrain_recipes CONFIGURE_DEPENDS
                 "${CMAKE_SOURCE_DIR}/${assets_rel}/terrain/*.terrain.json")
            foreach(recipe IN LISTS game_terrain_recipes)
                get_filename_component(recipe_file "${recipe}" NAME)
                string(REGEX REPLACE "\\.terrain\\.json$" "" recipe_stem "${recipe_file}")
                set(terrain_build_dir "${CMAKE_BINARY_DIR}/terrain/${game_name}")
                list(APPEND terrain_sync_commands COMMAND TerrainTool sync
                     --recipe "${recipe}" --id-root "${assets_rel}" --source-path "terrain/${recipe_file}"
                     --game-dir "${game_dir}" --candidate "${terrain_build_dir}/${recipe_stem}"
                     --report "${terrain_build_dir}/${recipe_stem}.sync.json")
                # The game reads games/ next to the exe: package each recipe's current release there.
                if(NOT MJOLNIR_WITH_EDITOR)
                    list(APPEND terrain_package_commands COMMAND TerrainTool package
                         --id-root "${assets_rel}" --source-path "terrain/${recipe_file}"
                         --game-dir "${game_dir}" --out "$<TARGET_FILE_DIR:MjolnirEngine>/games/${game_name}"
                         --report "${terrain_build_dir}/${recipe_stem}.package.json")
                endif()
            endforeach()
        endif()
    endif()

    list(APPEND game_copy_commands COMMAND "${CMAKE_COMMAND}" -E copy_if_different
         "${manifest}" "$<TARGET_FILE_DIR:MjolnirEngine>/games/${game_name}/game.json")

    # A game with terrain ships its streaming settings; terrain_streaming.local.json is per machine, never copied.
    if(EXISTS "${game_dir}/terrain_streaming.json")
        list(APPEND game_copy_commands COMMAND "${CMAKE_COMMAND}" -E copy_if_different
             "${game_dir}/terrain_streaming.json"
             "$<TARGET_FILE_DIR:MjolnirEngine>/games/${game_name}/terrain_streaming.json")
    endif()

    set(scene_dest "${CMAKE_BINARY_DIR}/games/${game_name}/scenes")
    if(IS_DIRECTORY "${game_dir}/scenes" AND NOT EXISTS "${scene_dest}")
        file(COPY "${game_dir}/scenes" DESTINATION "${CMAKE_BINARY_DIR}/games/${game_name}")
    endif()

    set(settings_src "${game_dir}/project_settings.json")
    set(settings_dest "${CMAKE_BINARY_DIR}/games/${game_name}/project_settings.json")
    if(EXISTS "${settings_src}" AND NOT EXISTS "${settings_dest}")
        file(COPY "${settings_src}" DESTINATION "${CMAKE_BINARY_DIR}/games/${game_name}")
    endif()
endforeach()

set(approved_asset_roots assets ${game_asset_roots})
engine_add_surface_materials(MjolnirEngine
    NAMESPACE default
    ROOT assets
    APPROVED_ROOTS ${approved_asset_roots})
foreach(game_name IN LISTS game_names)
    set(game_assets_root "games/${game_name}/assets")
    if(IS_DIRECTORY "${CMAKE_SOURCE_DIR}/${game_assets_root}")
        engine_add_surface_materials(MjolnirEngine
            NAMESPACE ${game_name}
            ROOT ${game_assets_root}
            APPROVED_ROOTS ${approved_asset_roots})
    endif()
endforeach()
engine_write_shader_manifest(MjolnirEngine)

# The reflection/manifest tests read the generated files, so a test build must
# produce the same shader outputs as a game build.
if(TARGET engine_renderer_tests)
    if(TARGET MjolnirEngine_surface_shaders_default)
        add_dependencies(engine_renderer_tests MjolnirEngine_surface_shaders_default)
    endif()
    foreach(game_name IN LISTS game_names)
        if(TARGET MjolnirEngine_surface_shaders_${game_name})
            add_dependencies(engine_renderer_tests MjolnirEngine_surface_shaders_${game_name})
        endif()
    endforeach()
endif()
# The ABI fixtures read real legacy reflections too.
if(TARGET engine_renderer_tests AND TARGET MjolnirEngine_shaders_raytrace)
    add_dependencies(engine_renderer_tests MjolnirEngine_shaders_raytrace)
endif()

engine_cook_assets(MjolnirEngine
    SOURCE_ROOTS assets/ ${game_asset_roots}
    OUT_DIR ${CMAKE_BINARY_DIR}/cooked_assets)
# They compare a one-game cook with this one
if(TARGET mjolnir_build_tests)
    add_dependencies(mjolnir_build_tests MjolnirEngine_cook_assets)
endif()

engine_copy_fonts(MjolnirEngine SOURCE_DIR fonts)
# The master logo and the icon compiled into the exe aren't read at runtime
engine_copy_branding(MjolnirEngine SOURCE_DIR branding EXCLUDE mjolnir-logo.png mjolnir.ico)
engine_write_player_info(MjolnirEngine)

# A build without the editor is the player; `cmake --install` turns it into a template
if(NOT MJOLNIR_WITH_EDITOR)
    engine_install_player(MjolnirEngine)
endif()

add_custom_target(MjolnirEngine_copy_games
    ${game_copy_commands}
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_SOURCE_DIR}/engine.json" "$<TARGET_FILE_DIR:MjolnirEngine>/engine.json"
    COMMENT "Copying game assets/manifests and engine.json next to MjolnirEngine"
    VERBATIM)
add_dependencies(MjolnirEngine MjolnirEngine_copy_games)

if(MIR_WITH_DLSS)
    add_custom_target(MjolnirEngine_copy_ngx
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${NGX_DLSSD_DLL}" "$<TARGET_FILE_DIR:MjolnirEngine>/nvngx_dlssd.dll"
        COMMENT "Copying NGX Ray Reconstruction runtime next to MjolnirEngine"
        VERBATIM)
    add_dependencies(MjolnirEngine MjolnirEngine_copy_ngx)
endif()

# Always runs; sync and package are no-ops while current.
if(terrain_sync_commands)
    add_custom_target(MjolnirEngine_sync_terrain
        ${terrain_sync_commands}
        ${terrain_package_commands}
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Publishing terrain recipes whose current release is out of date"
        VERBATIM)
    add_dependencies(MjolnirEngine_sync_terrain TerrainTool)
    add_dependencies(MjolnirEngine MjolnirEngine_sync_terrain)
endif()

find_package(nlohmann_json CONFIG REQUIRED)

target_link_libraries(MjolnirEngine PRIVATE
    mir::app
    mir::assets
    mir::core
    mir::gameshell
    mir::input
    mir::particles
    mir::particles_gpu
    mir::particles_runtime
    mir::particles_script
    mir::physics
    mir::prefab
    mir::project
    mir::scene
    mir::script
    mir::terrain
    nlohmann_json::nlohmann_json
)

# Smoke tests run each game for a few frames and fail on any logged error.
# They need a GPU; skip them with `ctest -LE smoke`.
# --non-interactive: a broken scene fails at once instead of waiting on its prompt until the timeout.
if(BUILD_TESTING)
    add_executable(shader_compiler_driver tools/tests/ShaderCompilerDriver.cpp)
    add_test(NAME shader_publication_fixture
        COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/tools/tests/ShaderPublicationFixture.ps1"
            -Repository "${CMAKE_SOURCE_DIR}" -BuildDirectory "${CMAKE_BINARY_DIR}"
            -Publisher "$<TARGET_FILE:ShaderPublisher>" -Compiler "$<TARGET_FILE:shader_compiler_driver>"
            -CxxCompiler "${CMAKE_CXX_COMPILER}" -MakeProgram "${CMAKE_MAKE_PROGRAM}")
    add_test(NAME surface_material_discovery
        COMMAND "${CMAKE_COMMAND}" -DSLANGC_EXECUTABLE=${SLANGC_EXECUTABLE}
                -P "${CMAKE_SOURCE_DIR}/tools/tests/SurfaceMaterialDiscovery.cmake")
    foreach(game_name IN LISTS game_names)
        add_test(NAME smoke.${game_name}
            COMMAND MjolnirEngine --non-interactive --hidden --game games/${game_name} --frames 120)
        set_tests_properties(smoke.${game_name} PROPERTIES
            LABELS smoke
            TIMEOUT 120)
    endforeach()

    # The game UI demo, which uploads the assets/ui/ art as ImGui textures and
    # frees it at shutdown
    if(EXISTS "${CMAKE_SOURCE_DIR}/games/HelloWorld/game.json")
        add_test(NAME smoke.ui-demo
            COMMAND MjolnirEngine --non-interactive --hidden --game games/HelloWorld --ui-demo --frames 120)
        set_tests_properties(smoke.ui-demo PROPERTIES
            LABELS smoke
            TIMEOUT 120)
    endif()

    # Game mode: no editor, so the game plays from the first frame, with the
    # HUD over it and its scripts running
    if(EXISTS "${CMAKE_SOURCE_DIR}/games/HelloWorld/game.json")
        add_test(NAME smoke.game-mode
            COMMAND MjolnirEngine --non-interactive --hidden --game games/HelloWorld --no-editor --frames 120)
        set_tests_properties(smoke.game-mode PROPERTIES
            LABELS smoke
            TIMEOUT 120)
    endif()

    # TerrainCheck from its scene camera, which sees both terrains: per-viewport selection, streaming and tracing
    if(EXISTS "${CMAKE_SOURCE_DIR}/games/TerrainCheck/game.json")
        add_test(NAME smoke.TerrainCheck.game-mode
            COMMAND MjolnirEngine --non-interactive --hidden --game games/TerrainCheck --no-editor --frames 120)
        set_tests_properties(smoke.TerrainCheck.game-mode PROPERTIES
            LABELS smoke
            TIMEOUT 120)
    endif()

    # SoundtrackCheck plays from the first frame, so its soundtrack is posted while its bank may still be loading
    if(EXISTS "${CMAKE_SOURCE_DIR}/games/SoundtrackCheck/game.json")
        add_test(NAME smoke.SoundtrackCheck.game-mode
            COMMAND MjolnirEngine --non-interactive --game games/SoundtrackCheck --no-editor --frames 120)
        set_tests_properties(smoke.SoundtrackCheck.game-mode PROPERTIES
            LABELS smoke
            TIMEOUT 120)
    endif()

    # Restarts HelloWorld -> EmptyScene -> HelloWorld, as Open Project does
    if(EXISTS "${CMAKE_SOURCE_DIR}/games/HelloWorld/game.json"
       AND EXISTS "${CMAKE_SOURCE_DIR}/games/EmptyScene/game.json")
        add_test(NAME smoke.switch
            COMMAND MjolnirEngine --non-interactive --hidden --game games/HelloWorld
                    --switch-to games/EmptyScene --switch-to games/HelloWorld
                    --frames 180)
        set_tests_properties(smoke.switch PROPERTIES
            LABELS smoke
            TIMEOUT 120)
    endif()

    # A game build installs itself as a player template, packages every game with
    # MjolnirBuild, then starts each package as a double-click would: no --game.
    # The packages go outside this build folder, where MjolnirBuild refuses to write
    if(NOT MJOLNIR_WITH_EDITOR)
        get_filename_component(build_name "${CMAKE_BINARY_DIR}" NAME)
        set(package_dir "${CMAKE_SOURCE_DIR}/builds/smoke-build/${build_name}")
        add_test(NAME build.player
            COMMAND "${CMAKE_COMMAND}" "-DBUILD_DIR=${CMAKE_BINARY_DIR}" "-DPREFIX=${package_dir}/player"
                    -P "${CMAKE_SOURCE_DIR}/tools/MjolnirBuild/tests/InstallPlayer.cmake")
        set_tests_properties(build.player PROPERTIES
            FIXTURES_SETUP build_player
            LABELS smoke
            TIMEOUT 300)
        foreach(game_name IN LISTS game_names)
            add_test(NAME build.package.${game_name}
                COMMAND MjolnirBuild --game "${CMAKE_SOURCE_DIR}/games/${game_name}" --player "${package_dir}/player"
                        --out "${package_dir}/${game_name}" --result "${package_dir}/${game_name}.json")
            set_tests_properties(build.package.${game_name} PROPERTIES
                FIXTURES_REQUIRED build_player
                FIXTURES_SETUP build_${game_name}
                LABELS smoke
                TIMEOUT 600)
            add_test(NAME smoke.build.${game_name}
                COMMAND "${CMAKE_COMMAND}" "-DRESULT=${package_dir}/${game_name}.json"
                        -P "${CMAKE_SOURCE_DIR}/tools/MjolnirBuild/tests/RunPackagedGame.cmake")
            set_tests_properties(smoke.build.${game_name} PROPERTIES
                FIXTURES_REQUIRED build_${game_name}
                LABELS smoke
                TIMEOUT 120)
        endforeach()
    endif()
endif()
