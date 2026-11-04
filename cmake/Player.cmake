# engine_write_player_info(<target>)
#
# Writes player_info.json next to <target> on every build: the engine version,
# commit, configuration and build options. A build tool compares the editor's
# copy with a player template's to tell whether they came from the same engine.
function(engine_write_player_info target)
    find_package(Git QUIET)

    set(flags "")
    foreach(option MJOLNIR_WITH_EDITOR MIR_LARGE_WORLD MIR_WITH_AUDIO MIR_WITH_DLSS MIR_WITH_RTXGI)
        if(${option})
            list(APPEND flags "-D${option}=true")
        else()
            list(APPEND flags "-D${option}=false")
        endif()
    endforeach()

    # A target with no OUTPUT runs every build, so the commit stays current without
    # a reconfigure; the script rewrites the file only when it changes.
    add_custom_target(${target}_player_info
        COMMAND "${CMAKE_COMMAND}"
            "-DOUT=$<TARGET_FILE_DIR:${target}>/player_info.json"
            "-DGIT=${GIT_EXECUTABLE}"
            "-DSOURCE_DIR=${CMAKE_SOURCE_DIR}"
            "-DENGINE_VERSION=${PROJECT_VERSION}"
            "-DCONFIG=${CMAKE_BUILD_TYPE}"
            ${flags}
            -P "${CMAKE_SOURCE_DIR}/cmake/PlayerInfo.cmake"
        COMMENT "Writing player_info.json next to ${target}"
        VERBATIM)
    add_dependencies(${target} ${target}_player_info)
endfunction()

# engine_install_player(<target>)
#
# Install rules for the player template:
#
#   cmake --install builds/windows-msvc-game-release --prefix builds/player/release
#
# Installs <target> and MjolnirErrorReporter with the DLLs they import, the
# current shader generation, fonts, branding and player_info.json. games/,
# cooked/ and engine.json depend on the game, so a build tool adds them.
function(engine_install_player target)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(vcpkg_bin "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/bin")
    else()
        set(vcpkg_bin "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin")
    endif()

    # Resolved from the executables' imports, not TARGET_RUNTIME_DLLS: LuaJIT comes
    # through pkg-config, whose imported target names no DLL.
    install(TARGETS ${target} MjolnirErrorReporter
        RUNTIME_DEPENDENCIES
            PRE_EXCLUDE_REGEXES "^api-ms-" "^ext-ms-"
            POST_EXCLUDE_REGEXES "[/\\\\][Ss][Yy][Ss][Tt][Ee][Mm]32[/\\\\]"
            DIRECTORIES "${vcpkg_bin}"
        RUNTIME DESTINATION .)

    # NGX loads it at runtime, so it is not an import
    if(MIR_WITH_DLSS)
        install(FILES "${NGX_DLSSD_DLL}" DESTINATION .)
    endif()

    install(DIRECTORY "$<TARGET_FILE_DIR:${target}>/fonts" "$<TARGET_FILE_DIR:${target}>/branding"
        DESTINATION .)
    install(FILES "$<TARGET_FILE_DIR:${target}>/player_info.json" DESTINATION .)

    # Only the generation current.json points at; the build tree keeps every older one.
    install(CODE "set(_mir_shader_root \"$<TARGET_FILE_DIR:${target}>/shaders\")")
    install(CODE [[
        if(NOT EXISTS "${_mir_shader_root}/current.json")
            message(FATAL_ERROR "No published shader generation in ${_mir_shader_root}. Build MjolnirEngine first.")
        endif()
        file(READ "${_mir_shader_root}/current.json" _mir_pointer)
        string(JSON _mir_generation GET "${_mir_pointer}" generation)

        # Zero-padded to 16 digits, as GenerationName in ShaderGeneration.cpp names it
        string(LENGTH "${_mir_generation}" _mir_length)
        math(EXPR _mir_padding "16 - ${_mir_length}")
        string(REPEAT "0" ${_mir_padding} _mir_zeros)

        file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/shaders/generations" TYPE DIRECTORY
            FILES "${_mir_shader_root}/generations/${_mir_zeros}${_mir_generation}")
        file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/shaders" TYPE FILE
            FILES "${_mir_shader_root}/current.json")
        if(IS_DIRECTORY "${_mir_shader_root}/rtxgi")
            file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/shaders" TYPE DIRECTORY
                FILES "${_mir_shader_root}/rtxgi")
        endif()
    ]])
endfunction()
