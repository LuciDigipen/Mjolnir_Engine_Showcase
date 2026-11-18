
# INCLUDE_DIR names the public include root, relative to the module; include by default.
# core uses it to build core_base and core from one folder with two roots
function(engine_add_module name)
    cmake_parse_arguments(ARG "" "INCLUDE_DIR" "PCH_HEADERS" ${ARGN})
    if(NOT ARG_INCLUDE_DIR)
        set(ARG_INCLUDE_DIR include)
    endif()

    # set a target with name engine_${name}
    set(target engine_${name})

    add_library(${target})
    add_library(mir::${name} ALIAS ${target})
    target_include_directories(${target} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/${ARG_INCLUDE_DIR})

    # base PCH headers
    foreach(header IN ITEMS
            algorithm array cmath cstdint filesystem format functional map memory
            optional span string string_view system_error unordered_map utility vector
            ${ARG_PCH_HEADERS})
        target_precompile_headers(${target} PRIVATE
            "$<$<COMPILE_LANGUAGE:CXX>:<${header}$<ANGLE-R>>")
    endforeach()

endfunction()

# Registers a CTest executable for a module
# Sources are relative to modules/<name>/tests/
# NO_SYSTEM_TIES skips the tie runs, for a suite that never runs a SystemScheduler
function(engine_add_module_tests name)
    if(NOT BUILD_TESTING)
        return()
    endif()

    cmake_parse_arguments(ARG "NO_SYSTEM_TIES" "" "" ${ARGN})
    set(target engine_${name}_tests)
    add_executable(${target} ${ARG_UNPARSED_ARGUMENTS})
    engine_target_utf8(${target})
    target_link_libraries(${target} PRIVATE mir::${name} Catch2::Catch2WithMain)

    # One CTest case per Catch2 TEST_CASE, so failures show up individually, not as one blob.
    catch_discover_tests(${target})

    # The whole suite again with equal SystemOrders reversed, then shuffled. A shuffle
    # prints its seed. Serial, so no case shares its temp files with its own discovered run
    if(NOT ARG_NO_SYSTEM_TIES)
        foreach(ties reverse shuffle)
            add_test(NAME ${target}.ties-${ties} COMMAND ${target})
            set_tests_properties(${target}.ties-${ties} PROPERTIES
                ENVIRONMENT "MIR_SYSTEM_TIES=${ties}"
                LABELS system-ties
                RUN_SERIAL TRUE
                SKIP_RETURN_CODE 4)
        endforeach()
    endif()
endfunction()
