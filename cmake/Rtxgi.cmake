include_guard(GLOBAL)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "RTXGI DDGI requires a 64-bit build.")
endif()

# Keep the SDK's top-level project out of our build: it configures sample
# dependencies and downloads DXC. Only fetch sources, without submodules.
include(FetchContent)
FetchContent_Declare(rtxgi_sdk
    GIT_REPOSITORY https://github.com/NVIDIAGameWorks/RTXGI-DDGI.git
    GIT_TAG f33e496ca31b3f0eec1c4e2cbaa8bb620e337fa6 # SDK 1.3.6
    GIT_SUBMODULES ""
    SOURCE_SUBDIR mjolnir-no-upstream-project)
FetchContent_MakeAvailable(rtxgi_sdk)
set(_rtxgi_root "${rtxgi_sdk_SOURCE_DIR}/rtxgi-sdk")

# Use the engine's existing Volk loader. The SDK's VulkanExtensions.cpp
# defines vk* wrapper functions that collide with Volk's function pointers.
add_library(engine_rtxgi STATIC
    "${_rtxgi_root}/src/Math.cpp"
    "${_rtxgi_root}/src/ddgi/DDGIVolume.cpp"
    "${_rtxgi_root}/src/ddgi/gfx/DDGIVolume_VK.cpp")
add_library(RTXGI::DDGI ALIAS engine_rtxgi)
target_include_directories(engine_rtxgi SYSTEM PUBLIC "${_rtxgi_root}/include")
target_link_libraries(engine_rtxgi PUBLIC volk::volk_headers)
target_compile_definitions(engine_rtxgi PUBLIC
    RTXGI_COORDINATE_SYSTEM=2 # Right handed, Y up; matches shaders.
    RTXGI_DDGI_RESOURCE_MANAGEMENT=0
    RTXGI_DDGI_USE_SHADER_CONFIG_FILE=0)
if(MSVC)
    target_compile_options(engine_rtxgi PRIVATE /FIvolk.h)
else()
    target_compile_options(engine_rtxgi PRIVATE -include volk.h)
endif()

# SDK HLSL uses DXC's explicit Vulkan bindings and push constants. Keep the
# engine's Slang path unchanged and require DXC only for this optional build.
find_program(RTXGI_DXC_EXECUTABLE NAMES dxc
    PATHS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin" NO_DEFAULT_PATH)
find_program(RTXGI_DXC_EXECUTABLE NAMES dxc)
if(NOT RTXGI_DXC_EXECUTABLE)
    message(FATAL_ERROR "MIR_WITH_RTXGI requires DXC with SPIR-V support. Set RTXGI_DXC_EXECUTABLE or install the Vulkan SDK.")
endif()

set(MIR_RTXGI_SHADER_DIR "${CMAKE_BINARY_DIR}/shaders/rtxgi")
set(_rtxgi_defines
    HLSL=1
    RTXGI_COORDINATE_SYSTEM=2
    RTXGI_DDGI_RESOURCE_MANAGEMENT=0
    RTXGI_DDGI_USE_SHADER_CONFIG_FILE=0
    RTXGI_DDGI_SHADER_REFLECTION=0
    RTXGI_DDGI_BINDLESS_RESOURCES=0
    RTXGI_PUSH_CONSTS_TYPE=1
    VOLUME_CONSTS_REGISTER=0 VOLUME_CONSTS_SPACE=0
    RAY_DATA_REGISTER=1 RAY_DATA_SPACE=0
    OUTPUT_SPACE=0
    PROBE_DATA_REGISTER=4 PROBE_DATA_SPACE=0
    PROBE_VARIABILITY_REGISTER=5 PROBE_VARIABILITY_SPACE=0
    RTXGI_DDGI_BLEND_SHARED_MEMORY=0
    RTXGI_DDGI_DEBUG_PROBE_INDEXING=0
    RTXGI_DDGI_DEBUG_OCTAHEDRAL_INDEXING=0
    RTXGI_DDGI_BLEND_SCROLL_SHARED_MEMORY=0)
set(_rtxgi_define_args "")
foreach(define IN LISTS _rtxgi_defines)
    list(APPEND _rtxgi_define_args "-D${define}")
endforeach()
# SDK shader includes reach into include/rtxgi; track both trees so header
# changes trigger recompilation, including with a local FetchContent override.
file(GLOB_RECURSE _rtxgi_shader_deps CONFIGURE_DEPENDS
    "${_rtxgi_root}/shaders/*.hlsl" "${_rtxgi_root}/include/*.h")

function(_mir_rtxgi_shader name source entry)
    set(output "${MIR_RTXGI_SHADER_DIR}/${name}.spv")
    add_custom_command(OUTPUT "${output}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${MIR_RTXGI_SHADER_DIR}"
        COMMAND "${RTXGI_DXC_EXECUTABLE}" "${_rtxgi_root}/shaders/ddgi/${source}.hlsl"
            -spirv -fspv-target-env=vulkan1.2 -T cs_6_0 -E "${entry}"
            ${_rtxgi_define_args} ${ARGN} -Fo "${output}"
        DEPENDS ${_rtxgi_shader_deps}
        COMMENT "DXC RTXGI ${name}"
        VERBATIM COMMAND_EXPAND_LISTS)
    set_property(GLOBAL APPEND PROPERTY MIR_RTXGI_SHADER_OUTPUTS "${output}")
endfunction()

# No shared-memory ray-count specialization: later settings may vary rays per
# probe without rebuilding shaders. Atlas interiors are 6/14 texels (+2 border).
_mir_rtxgi_shader(ProbeIrradiance ProbeBlendingCS DDGIProbeBlendingCS
    -DRTXGI_DDGI_BLEND_RADIANCE=1 -DOUTPUT_REGISTER=2
    -DRTXGI_DDGI_PROBE_NUM_TEXELS=8 -DRTXGI_DDGI_PROBE_NUM_INTERIOR_TEXELS=6)
_mir_rtxgi_shader(ProbeDistance ProbeBlendingCS DDGIProbeBlendingCS
    -DRTXGI_DDGI_BLEND_RADIANCE=0 -DOUTPUT_REGISTER=3
    -DRTXGI_DDGI_PROBE_NUM_TEXELS=16 -DRTXGI_DDGI_PROBE_NUM_INTERIOR_TEXELS=14)
_mir_rtxgi_shader(ProbeRelocation ProbeRelocationCS DDGIProbeRelocationCS)
_mir_rtxgi_shader(ProbeRelocationReset ProbeRelocationCS DDGIProbeRelocationResetCS)
_mir_rtxgi_shader(ProbeClassification ProbeClassificationCS DDGIProbeClassificationCS)
_mir_rtxgi_shader(ProbeClassificationReset ProbeClassificationCS DDGIProbeClassificationResetCS)
get_property(_rtxgi_outputs GLOBAL PROPERTY MIR_RTXGI_SHADER_OUTPUTS)
add_custom_target(engine_rtxgi_shaders DEPENDS ${_rtxgi_outputs})
add_dependencies(engine_rtxgi engine_rtxgi_shaders)

message(STATUS "RTXGI DDGI: SDK 1.3.6 (f33e496), renderer-owned resources, DXC ${RTXGI_DXC_EXECUTABLE}")
