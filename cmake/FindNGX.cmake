if(TARGET NGX::NGX)
    return()
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "NGX/DLSS-RR requires a 64-bit build.")
endif()

# Public repo - EULA governs usage, not access, so FetchContent can clone it
# with no login/clickthrough. Pinned to an exact release tag; never committed,
# lands under the build tree like any other FetchContent dependency.
include(FetchContent)
FetchContent_Declare(ngx_sdk
    GIT_REPOSITORY https://github.com/NVIDIA/DLSS.git
    GIT_TAG v310.9.16
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(ngx_sdk)

set(_ngx_platform "Windows_x86_64")
set(_ngx_lib_dir "${ngx_sdk_SOURCE_DIR}/lib/${_ngx_platform}")

# "_d"/"_d_dbg", not "_s"/"_s_dbg": the SDK ships two full sets of import libs per config -
# "_s" is built against the static CRT (/MT, /MTd) and "_d" against the dynamic CRT (/MD, /MDd).
# "_s" is the one every public NGX sample shows first, but it embeds a #pragma detect_mismatch
# CRT tag that hard-fails (LNK2038, not bypassable via /NODEFAULTLIB or /FORCE - confirmed by
# trying both) against this project's /MD(d), same as every vcpkg dependency here. "_d" carries
# the matching tag instead and links clean.
add_library(NGX::NGX STATIC IMPORTED)
set_target_properties(NGX::NGX PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${ngx_sdk_SOURCE_DIR}/include"
    IMPORTED_LOCATION "${_ngx_lib_dir}/x64/nvsdk_ngx_d.lib"
    IMPORTED_LOCATION_DEBUG "${_ngx_lib_dir}/x64/nvsdk_ngx_d_dbg.lib")

# The runtime DLL that actually carries the Ray Reconstruction model - the
# import lib above only gets the application into the driver's NGX loader.
# "rel" (not "dev") matches a Release NGX log verbosity regardless of our own
# build configuration; "dev" is for NGX-side debugging, not ours.
set(NGX_DLSSD_DLL "${_ngx_lib_dir}/rel/nvngx_dlssd.dll" CACHE INTERNAL "")

message(STATUS "NGX/DLSS-RR SDK: ${ngx_sdk_SOURCE_DIR} (${_ngx_platform}, v310.9.16)")
