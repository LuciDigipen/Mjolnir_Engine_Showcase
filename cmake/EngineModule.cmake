
# INCLUDE_DIR names the public include root, relative to the module; include by default.
# core uses it to build core_base and core from one folder with two roots
function(engine_add_module name)
    cmake_parse_arguments(ARG "" "INCLUDE_DIR" "" ${ARGN})
    if(NOT ARG_INCLUDE_DIR)
        set(ARG_INCLUDE_DIR include)
    endif()

    # set a target with name engine_${name}
    set(target engine_${name})

    add_library(${target})
    add_library(mir::${name} ALIAS ${target})
    target_include_directories(${target} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/${ARG_INCLUDE_DIR})

endfunction()

# Registers a CTest executable for a module
# Sources are relative to modules/<name>/tests/
function(engine_add_module_tests name)
    if(NOT BUILD_TESTING)
        return()
    endif()

    set(target engine_${name}_tests)
    add_executable(${target} ${ARGN})
    target_link_libraries(${target} PRIVATE mir::${name} Catch2::Catch2WithMain)

    # One CTest case per Catch2 TEST_CASE, so failures show up individually, not as one blob.
    catch_discover_tests(${target})
endfunction()