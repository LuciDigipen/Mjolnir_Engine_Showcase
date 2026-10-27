cmake_minimum_required(VERSION 3.26)
project(SurfaceMaterialFixture LANGUAGES CXX)

# The fixture invokes the real engine_add_surface_materials function, but its
# own source root is not the engine root.
get_filename_component(MIR_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(SLANGC_EXECUTABLE "${SLANGC_EXECUTABLE}" CACHE FILEPATH "")
include("${MIR_REPO_ROOT}/cmake/Shaders.cmake")

set(FIXTURE_ASSETS "${CMAKE_CURRENT_SOURCE_DIR}/assets" CACHE PATH "")
add_library(surface_fixture STATIC fixture.cpp)
engine_add_surface_materials(surface_fixture
    NAMESPACE fixture
    ROOT "${FIXTURE_ASSETS}"
    APPROVED_ROOTS "${FIXTURE_ASSETS}")
engine_write_shader_manifest(surface_fixture)
