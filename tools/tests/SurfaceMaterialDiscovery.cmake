if(NOT SLANGC_EXECUTABLE)
    set(SLANGC_EXECUTABLE "slangc" CACHE STRING "" FORCE)
endif()
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/Shaders.cmake")

function(expect_equal actual expected label)
    if(NOT "${actual}" STREQUAL "${expected}")
        message(FATAL_ERROR "${label}: expected '${expected}', got '${actual}'")
    endif()
endfunction()

function(expect_different first second label)
    if("${first}" STREQUAL "${second}")
        message(FATAL_ERROR "${label}: values unexpectedly match '${first}'")
    endif()
endfunction()

_mir_surface_identity(default_stem default_key default DefaultMaterial assets/materials/default.slang mesh)
expect_equal("${default_stem}" "default_DefaultMaterial_3610072657" "CMake and runtime stem hashing agree")

_mir_surface_identity(shared_a key_a engine TypeA assets/materials/shared.slang mesh)
_mir_surface_identity(shared_b key_b engine TypeA assets/materials/shared.slang mesh)
expect_equal("${key_a}" "${key_b}" "two assets reusing one shader share a compile key")
expect_equal("${shared_a}" "${shared_b}" "two assets reusing one shader share a stem")

_mir_surface_identity(other key_other engine TypeA assets/materials/other.slang mesh)
expect_different("${key_a}" "${key_other}" "same type from different sources")

_mir_surface_identity(upper key_upper engine TypeA assets/Materials/Shared.slang mesh)
expect_equal("${key_a}" "${key_upper}" "case variants normalize to one source identity")

_mir_surface_identity(dash key_dash engine A-B assets/materials/shared.slang mesh)
_mir_surface_identity(underscore key_underscore engine A_B assets/materials/shared.slang mesh)
expect_different("${key_dash}" "${key_underscore}" "punctuation variants stay distinct")
expect_different("${dash}" "${underscore}" "punctuation variants get distinct stems")

_mir_surface_identity(terrain key_terrain engine TypeA assets/materials/shared.slang terrain)
_mir_surface_identity(mesh key_mesh engine TypeA assets/materials/shared.slang mesh)
expect_different("${key_terrain}" "${key_mesh}" "frontend is part of the key")
expect_equal("${shared_a}" "${mesh}" "a later mesh registration still uses the mesh stem")

message(STATUS "Surface material discovery fixtures passed")
