if(NOT DEFINED MIR_REPO_ROOT)
    set(MIR_REPO_ROOT "${CMAKE_SOURCE_DIR}")
endif()

# Locate slangc once, at configure time.
# VULKAN_SDK first: the SDK's slangc is version-matched to its validation
# layers. A stray slangc on PATH may be older than the SDK.
find_program(SLANGC_EXECUTABLE
    NAMES slangc
    PATHS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin"
    NO_DEFAULT_PATH)

# Second pass: allow PATH if the SDK layout differs.
find_program(SLANGC_EXECUTABLE NAMES slangc)

if(NOT SLANGC_EXECUTABLE)
    # No glslc fallback on purpose: a silent fallback means compiling a
    # different language than the source is written in.
    message(FATAL_ERROR
        "slangc not found. Install the Vulkan SDK (which ships it) and ensure "
        "VULKAN_SDK is set, or put slangc on PATH.")
endif()

message(STATUS "slangc: ${SLANGC_EXECUTABLE}")

if(CMAKE_BUILD_TYPE STREQUAL "Debug" OR NOT CMAKE_BUILD_TYPE)
    set(MIR_SHADER_OPTIMIZATION_FLAGS -g -O0)
else()
    set(MIR_SHADER_OPTIMIZATION_FLAGS -O3)
endif()

set(MIR_SHADER_COMMON_ARGS
    -target spirv
    -profile spirv_1_5
    -fvk-use-entrypoint-name
    -matrix-layout-column-major
    ${MIR_SHADER_OPTIMIZATION_FLAGS})

set_property(GLOBAL PROPERTY MIR_SHADER_MANIFEST_OUTPUTS "")
set_property(GLOBAL PROPERTY MIR_SHADER_MANIFEST_TEMPLATES "")
set_property(GLOBAL PROPERTY MIR_SHADER_ASSET_ROOTS "")
set_property(GLOBAL PROPERTY MIR_SHADER_GENERATED_OUTPUTS "")
set_property(GLOBAL PROPERTY MIR_SURFACE_CATALOG_ENTRIES "")
set_property(GLOBAL PROPERTY MIR_SURFACE_COMPILED_KEYS "")
set_property(GLOBAL PROPERTY MIR_SURFACE_COMPILED_STEMS "")

function(_mir_json_escape out_var value)
    string(REPLACE "\\" "\\\\" escaped "${value}")
    string(REPLACE "\"" "\\\"" escaped "${escaped}")
    string(REPLACE "\n" "\\n" escaped "${escaped}")
    set(${out_var} "${escaped}" PARENT_SCOPE)
endfunction()

# Standalone source fixtures may supply the already-built tool.
if(MIR_SHADER_PUBLISHER AND NOT TARGET ShaderPublisher)
    add_executable(ShaderPublisher IMPORTED GLOBAL)
    set_property(TARGET ShaderPublisher PROPERTY IMPORTED_LOCATION "${MIR_SHADER_PUBLISHER}")
endif()

function(_mir_compile_command out_var output)
    if(TARGET ShaderPublisher)
        get_filename_component(name "${output}" NAME)
        set(command "$<TARGET_FILE:ShaderPublisher>"
            --manifest "${CMAKE_BINARY_DIR}/shader_build/shader_manifest.json"
            --cache "${CMAKE_BINARY_DIR}/shader_build" --compile "${name}")
    else()
        set(command ${ARGN})
    endif()
    set(${out_var} ${command} PARENT_SCOPE)
endfunction()

function(_mir_normalize_logical out_var value)
    file(TO_CMAKE_PATH "${value}" normalized)
    string(TOLOWER "${normalized}" normalized)
    string(REGEX REPLACE "^\\./+" "" normalized "${normalized}")
    set(${out_var} "${normalized}" PARENT_SCOPE)
endfunction()

function(_mir_fnv1a32 out_var text)
    set(hash 2166136261)
    string(LENGTH "${text}" length)
    if(length GREATER 0)
        math(EXPR last "${length} - 1")
        foreach(index RANGE ${last})
            string(SUBSTRING "${text}" ${index} 1 character)
            string(HEX "${character}" hex)
            string(LENGTH "${hex}" hex_length)
            math(EXPR bytes "${hex_length} / 2")
            if(bytes GREATER 0)
                math(EXPR last_byte "${bytes} - 1")
                foreach(byte_index RANGE ${last_byte})
                    math(EXPR offset "${byte_index} * 2")
                    string(SUBSTRING "${hex}" ${offset} 2 byte)
                    math(EXPR code "0x${byte}")
                    math(EXPR hash "${hash} ^ ${code}")
                    math(EXPR hash "${hash} * 16777619")
                    math(EXPR hash "${hash} & 0xFFFFFFFF")
                endforeach()
            endif()
        endforeach()
    endif()
    set(${out_var} "${hash}" PARENT_SCOPE)
endfunction()

# The compile key is the normalized shader source identity plus type and
# frontend; the stem carries the same hash, so two games or two source files
# cannot alias even when punctuation sanitizes to the same text.
function(_mir_surface_identity out_stem out_key name_space type shader_relative frontend)
    _mir_normalize_logical(shader_norm "${shader_relative}")
    set(key "${shader_norm}|${type}|${frontend}")
    _mir_fnv1a32(hash "${key}")
    string(REGEX REPLACE "[^A-Za-z0-9_]" "_" type_clean "${type}")
    if(name_space)
        set(stem "${name_space}_${type_clean}_${hash}")
    else()
        set(stem "${type_clean}_${hash}")
    endif()
    set(${out_stem} "${stem}" PARENT_SCOPE)
    set(${out_key} "${key}" PARENT_SCOPE)
endfunction()

function(_mir_json_array out_var)
    set(result "")
    foreach(item IN LISTS ARGN)
        _mir_json_escape(item "${item}")
        if(result STREQUAL "")
            set(result "\"${item}\"")
        else()
            set(result "${result}, \"${item}\"")
        endif()
    endforeach()
    set(${out_var} "[${result}]" PARENT_SCOPE)
endfunction()

# Records one output in the same list engine_write_shader_manifest serializes.
function(_mir_manifest_add_output)
    cmake_parse_arguments(ARG "" "OUTPUT;ENTRY;STAGE;TYPE;SPECIALIZE;REFLECTION;DEPFILE;FRONTEND"
        "SOURCES;ARGUMENTS" ${ARGN})

    if(NOT ARG_OUTPUT OR NOT ARG_ENTRY OR NOT ARG_STAGE OR NOT ARG_TYPE OR NOT ARG_SOURCES)
        message(FATAL_ERROR "_mir_manifest_add_output: OUTPUT, ENTRY, STAGE, TYPE and SOURCES are required")
    endif()

    _mir_json_array(sources_json ${ARG_SOURCES})
    _mir_json_array(arguments_json ${ARG_ARGUMENTS})
    _mir_json_escape(output_json "${ARG_OUTPUT}")
    _mir_json_escape(entry_json "${ARG_ENTRY}")
    _mir_json_escape(stage_json "${ARG_STAGE}")
    _mir_json_escape(type_json "${ARG_TYPE}")
    _mir_json_escape(specialize_json "${ARG_SPECIALIZE}")
    _mir_json_escape(reflection_json "${ARG_REFLECTION}")
    _mir_json_escape(depfile_json "${ARG_DEPFILE}")
    _mir_json_escape(frontend_json "${ARG_FRONTEND}")

    string(CONCAT entry
        "{\n    \"type\": \"${type_json}\",\n    \"frontend\": \"${frontend_json}\",\n"
        "\"sources\": ${sources_json},\n    \"entry\": \"${entry_json}\",\n"
        "\"stage\": \"${stage_json}\",\n    \"specialize\": \"${specialize_json}\",\n"
        "\"output\": \"${output_json}\",\n    \"reflection\": \"${reflection_json}\",\n"
        "\"depfile\": \"${depfile_json}\",\n    \"arguments\": ${arguments_json}\n  }")

    set_property(GLOBAL APPEND PROPERTY MIR_SHADER_MANIFEST_OUTPUTS "${entry}")
endfunction()

# engine_write_shader_manifest(<target>)
#
# Writes the one command source for the build and the editor's SPV reload:
# shaders/shader_manifest.json next to the generated .spv files.
function(engine_write_shader_manifest target)
    get_property(outputs GLOBAL PROPERTY MIR_SHADER_MANIFEST_OUTPUTS)
    get_property(templates GLOBAL PROPERTY MIR_SHADER_MANIFEST_TEMPLATES)
    get_property(asset_roots GLOBAL PROPERTY MIR_SHADER_ASSET_ROOTS)
    get_property(catalog_entries GLOBAL PROPERTY MIR_SURFACE_CATALOG_ENTRIES)
    _mir_json_array(common_json ${MIR_SHADER_COMMON_ARGS})
    _mir_json_escape(slangc_json "${SLANGC_EXECUTABLE}")
    _mir_json_escape(working_json "${MIR_REPO_ROOT}")

    string(JOIN ",\n    " outputs_json ${outputs})
    string(JOIN ",\n    " templates_json ${templates})
    string(JOIN ",\n    " asset_roots_json ${asset_roots})
    string(JOIN ",\n    " catalog_json ${catalog_entries})

    set(manifest "${CMAKE_BINARY_DIR}/shader_build/shader_manifest.json")
    string(CONCAT manifest_json
        "{\n  \"version\": 1,\n  \"slangc\": \"${slangc_json}\",\n"
        "  \"workingDirectory\": \"${working_json}\",\n  \"commonArguments\": ${common_json},\n"
        "  \"assetRoots\": [\n    ${asset_roots_json}\n  ],\n"
        "  \"outputs\": [\n    ${outputs_json}\n  ],\n"
        "  \"frontendTemplates\": [\n    ${templates_json}\n  ]\n}\n")
    file(GENERATE OUTPUT "${manifest}" CONTENT "${manifest_json}")

    set(catalog "${CMAKE_BINARY_DIR}/shader_build/surface_materials.json")
    string(CONCAT catalog_file
        "{\n  \"version\": 1,\n  \"materials\": [\n    ${catalog_json}\n  ]\n}\n")
    file(GENERATE OUTPUT "${catalog}" CONTENT "${catalog_file}")

    if(TARGET ShaderPublisher)
        get_property(generated GLOBAL PROPERTY MIR_SHADER_GENERATED_OUTPUTS)
        add_custom_target(${target}_publish_shaders
            COMMAND "$<TARGET_FILE:ShaderPublisher>" --manifest "${manifest}" --catalog "${catalog}"
                    --cache "${CMAKE_BINARY_DIR}/shader_build" --root "$<TARGET_FILE_DIR:${target}>/shaders"
            DEPENDS ShaderPublisher ${generated}
            COMMENT "Checking shader content and publishing complete generations"
            VERBATIM)
        add_dependencies(${target} ${target}_publish_shaders)
    endif()
endfunction()

# Recursively collects the files a Slang source #include"s
function(_engine_scan_shader_includes source_file out_var)
    set(visited "")
    set(pending "${source_file}")
    set(result "")

    while(pending)
        list(GET pending 0 current)
        list(REMOVE_AT pending 0)

        if(current IN_LIST visited)
            continue()
        endif()
        list(APPEND visited "${current}")

        if(NOT EXISTS "${current}")
            continue()
        endif()

        get_filename_component(current_dir "${current}" DIRECTORY)

        file(STRINGS "${current}" include_lines
            REGEX "^[ \t]*#include[ \t]+\"[^\"]+\"")

        foreach(line IN LISTS include_lines)
            string(REGEX REPLACE "^[ \t]*#include[ \t]+\"([^\"]+)\".*" "\\1"
                included "${line}")
            get_filename_component(included_abs "${current_dir}/${included}" ABSOLUTE)
            if(NOT EXISTS "${included_abs}")
                foreach(include_dir IN LISTS ARGN)
                    if(EXISTS "${include_dir}/${included}")
                        get_filename_component(included_abs "${include_dir}/${included}" ABSOLUTE)
                        break()
                    endif()
                endforeach()
            endif()
            # Conditional SDK includes may refer to optional config files.
            # The compiler diagnoses unresolved active includes.
            if(EXISTS "${included_abs}")
                list(APPEND result "${included_abs}")
                list(APPEND pending "${included_abs}")
            endif()
        endforeach()
    endwhile()

    set(${out_var} "${result}" PARENT_SCOPE)
endfunction()

# engine_add_shaders(<target>
#     SOURCE  <file.slang>
#     ENTRIES <name>:<stage> [<name>:<stage> ...])
#
# Emits one .spv per entry point, then copies them next to the executable so
# the run directory is self-contained and the exe works when moved.
function(engine_add_shaders target)
    cmake_parse_arguments(ARG "" "SOURCE" "ENTRIES;INCLUDE_DIRS;DEFINES;EXTRA_DEPENDS" ${ARGN})

    if(NOT ARG_SOURCE)
        message(FATAL_ERROR "engine_add_shaders: SOURCE is required")
    endif()

    if(NOT ARG_ENTRIES)
        message(FATAL_ERROR "engine_add_shaders: ENTRIES is required")
    endif()

    if(ARG_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "engine_add_shaders: unrecognised arguments (keywords are "
            "case-sensitive): ${ARG_UNPARSED_ARGUMENTS}")
    endif()

    get_filename_component(source_abs "${ARG_SOURCE}" ABSOLUTE)
    get_filename_component(source_name "${ARG_SOURCE}" NAME_WE)

    _engine_scan_shader_includes("${source_abs}" included_deps ${ARG_INCLUDE_DIRS})

    # No generator expression in this path: add_custom_command(OUTPUT) paths
    # containing a genex cannot be matched against add_custom_target(DEPENDS),
    # so the command silently never runs.
    set(gen_dir "${CMAKE_BINARY_DIR}/shader_build")

    set(outputs "")

    # MIR_SHADER_COMMON_ARGS already carries the configuration's -g -O0 / -O3.
    set(extra_flags "")
    foreach(directory IN LISTS ARG_INCLUDE_DIRS)
        list(APPEND extra_flags -I "${directory}")
    endforeach()
    foreach(define IN LISTS ARG_DEFINES)
        list(APPEND extra_flags "-D${define}")
    endforeach()

    foreach(entry IN LISTS ARG_ENTRIES)
        string(REPLACE ":" ";" parts "${entry}")
        list(GET parts 0 entry_name)
        list(GET parts 1 entry_stage)

        set(spv "${gen_dir}/${source_name}.${entry_name}.spv")
        set(reflection "${spv}.reflection.json")
        set(depfile "${spv}.d")

        set(arguments "${SLANGC_EXECUTABLE}" "${source_abs}" ${MIR_SHADER_COMMON_ARGS} ${extra_flags}
            -entry "${entry_name}" -stage "${entry_stage}"
            -reflection-json "${reflection}" -depfile "${depfile}" -o "${spv}")
        _mir_compile_command(compile_command "${spv}" ${arguments})

        add_custom_command(
            OUTPUT "${spv}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${gen_dir}"
            COMMAND ${compile_command}
            # DEPENDS on the source (plus anything it #includes,
            # transitively) gives recompile-on-edit for free.
            DEPENDS "${source_abs}" ${included_deps} ${ARG_EXTRA_DEPENDS}
            DEPFILE "${depfile}"
            BYPRODUCTS "${reflection}"
            COMMENT "slangc ${source_name}.slang [${entry_name}/${entry_stage}]"
            VERBATIM
            COMMAND_EXPAND_LISTS)
        set_property(GLOBAL APPEND PROPERTY MIR_SHADER_GENERATED_OUTPUTS "${spv}")
        _mir_manifest_add_output(
            OUTPUT "${spv}"
            SOURCES "${source_abs}"
            ENTRY "${entry_name}"
            STAGE "${entry_stage}"
            TYPE legacy
            REFLECTION "${reflection}"
            DEPFILE "${depfile}"
            ARGUMENTS ${arguments})

        list(APPEND outputs "${spv}")
    endforeach()

    # A custom target to manage the shaders.
    # shaders are built before the exe links and rebuilt when edited.
    set(shader_target ${target}_shaders_${source_name})
    add_custom_target(${shader_target} DEPENDS ${outputs})
    add_dependencies(${target} ${shader_target})

endfunction()

function(_mir_surface_template_registered out_var)
    get_property(templates GLOBAL PROPERTY MIR_SHADER_MANIFEST_TEMPLATES)
    if(templates)
        set(${out_var} TRUE PARENT_SCOPE)
    else()
        set(${out_var} FALSE PARENT_SCOPE)
    endif()
endfunction()

function(_mir_register_mesh_frontend_template)
    get_property(frontends GLOBAL PROPERTY MIR_SHADER_MANIFEST_FRONTENDS)
    if("mesh" IN_LIST frontends)
        return()
    endif()
    set_property(GLOBAL APPEND PROPERTY MIR_SHADER_MANIFEST_FRONTENDS mesh)

    set(template "{\n    \"frontend\": \"mesh\",\n    \"placeholders\": [\"{surfaceForward}\", \"{surfaceRt}\", \"{surfaceLayout}\", \"{materialSource}\", \"{type}\", \"{stem}\"],\n    \"vertex\": [\"{surfaceForward}\", \"{materialSource}\", \"-fvk-use-gl-layout\", \"-entry\", \"vertexMain\", \"-stage\", \"vertex\"],\n    \"fragment\": [\"{surfaceForward}\", \"{materialSource}\", \"-fvk-use-gl-layout\", \"-entry\", \"fragmentMain\", \"-stage\", \"fragment\", \"-specialize\", \"{type}\"],\n    \"surfaceFragment\": [\"{surfaceForward}\", \"{materialSource}\", \"-fvk-use-gl-layout\", \"-DMIR_SSGI_SURFACE\", \"-entry\", \"fragmentMain\", \"-stage\", \"fragment\", \"-specialize\", \"{type}\"],\n    \"raytracing\": [\"{surfaceRt}\", \"{materialSource}\", \"-fvk-use-gl-layout\", \"-entry\", \"closestHitSurface\", \"-stage\", \"closesthit\", \"-specialize\", \"{type}\"],\n    \"layout\": [\"{surfaceLayout}\", \"{materialSource}\", \"-fvk-use-gl-layout\", \"-entry\", \"layoutMain\", \"-stage\", \"compute\", \"-specialize\", \"{type}\"]\n  }")
    string(JSON template SET "${template}" placeholders "[\"{materialSource}\",\"{type}\",\"{stem}\"]")
    foreach(frontend_source IN ITEMS Forward Rt Layout)
        _mir_json_escape(frontend_path "${MIR_REPO_ROOT}/assets/shaders/Surface${frontend_source}.slang")
        string(REPLACE "{surface${frontend_source}}" "${frontend_path}" template "${template}")
    endforeach()
    if(MIR_WITH_RTXGI)
        string(JSON gi_args GET "${template}" raytracing)
        string(JSON arg_count LENGTH "${gi_args}")
        string(JSON gi_args SET "${gi_args}" ${arg_count} "\"-DMIR_DDGI\"")
        string(JSON template SET "${template}" giRaytracing "${gi_args}")
        math(EXPR arg_count "${arg_count} + 1")
        string(JSON gi_args SET "${gi_args}" ${arg_count} "\"-DMIR_DDGI_PROBE\"")
        string(JSON template SET "${template}" probeRaytracing "${gi_args}")
    endif()
    set_property(GLOBAL APPEND PROPERTY MIR_SHADER_MANIFEST_TEMPLATES "${template}")
endfunction()

function(_mir_register_terrain_frontend_template)
    get_property(frontends GLOBAL PROPERTY MIR_SHADER_MANIFEST_FRONTENDS)
    if("terrain" IN_LIST frontends)
        return()
    endif()
    set_property(GLOBAL APPEND PROPERTY MIR_SHADER_MANIFEST_FRONTENDS terrain)

    set(template "{\n    \"frontend\": \"terrain\",\n    \"placeholders\": [\"{surfaceForward}\", \"{surfaceRt}\", \"{surfaceLayout}\", \"{materialSource}\", \"{type}\", \"{stem}\"],\n    \"vertex\": [\"{surfaceForward}\", \"{materialSource}\", \"-fvk-use-gl-layout\", \"-entry\", \"vertexMain\", \"-stage\", \"vertex\"],\n    \"fragment\": [\"{surfaceForward}\", \"{materialSource}\", \"-fvk-use-gl-layout\", \"-entry\", \"fragmentMain\", \"-stage\", \"fragment\", \"-specialize\", \"{type}\"],\n    \"surfaceFragment\": [\"{surfaceForward}\", \"{materialSource}\", \"-fvk-use-gl-layout\", \"-DMIR_SSGI_SURFACE\", \"-entry\", \"fragmentMain\", \"-stage\", \"fragment\", \"-specialize\", \"{type}\"],\n    \"raytracing\": [\"{surfaceRt}\", \"{materialSource}\", \"-fvk-use-gl-layout\", \"-entry\", \"closestHitSurface\", \"-stage\", \"closesthit\", \"-specialize\", \"{type}\"],\n    \"layout\": [\"{surfaceLayout}\", \"{materialSource}\", \"-fvk-use-gl-layout\", \"-entry\", \"layoutMain\", \"-stage\", \"compute\", \"-specialize\", \"{type}\"]\n  }")
    string(JSON template SET "${template}" placeholders "[\"{materialSource}\",\"{type}\",\"{stem}\"]")
    foreach(frontend_source IN ITEMS Forward Rt Layout)
        if(frontend_source STREQUAL "Layout")
            set(frontend_file "SurfaceLayout.slang")
        else()
            set(frontend_file "SurfaceTerrain${frontend_source}.slang")
        endif()
        _mir_json_escape(frontend_path "${MIR_REPO_ROOT}/assets/shaders/${frontend_file}")
        string(REPLACE "{surface${frontend_source}}" "${frontend_path}" template "${template}")
    endforeach()
    if(MIR_WITH_RTXGI)
        string(JSON gi_args GET "${template}" raytracing)
        string(JSON arg_count LENGTH "${gi_args}")
        string(JSON gi_args SET "${gi_args}" ${arg_count} "\"-DMIR_DDGI\"")
        string(JSON template SET "${template}" giRaytracing "${gi_args}")
        math(EXPR arg_count "${arg_count} + 1")
        string(JSON gi_args SET "${gi_args}" ${arg_count} "\"-DMIR_DDGI_PROBE\"")
        string(JSON template SET "${template}" probeRaytracing "${gi_args}")
    endif()
    set_property(GLOBAL APPEND PROPERTY MIR_SHADER_MANIFEST_TEMPLATES "${template}")
endfunction()

# engine_add_surface_materials(<target>
#     NAMESPACE <game-or-default>
#     ROOT <repo-relative assets root>
#     APPROVED_ROOTS <root> [...])
#
# Discovers <ROOT>/materials/*.material.json, deduplicates by source/type/frontend,
# and compiles the mesh raster, RT, vertex and layout
# outputs through the fixed frontend sources. A new material only needs the
# JSON and its type; no wrapper source or CMake edit.
function(engine_add_surface_materials target)
    cmake_parse_arguments(ARG "" "NAMESPACE;ROOT" "APPROVED_ROOTS" ${ARGN})
    if(NOT ARG_NAMESPACE OR NOT ARG_ROOT)
        message(FATAL_ERROR "engine_add_surface_materials: NAMESPACE and ROOT are required")
    endif()

    get_filename_component(root_abs "${ARG_ROOT}" ABSOLUTE BASE_DIR "${MIR_REPO_ROOT}")
    file(RELATIVE_PATH root_relative "${MIR_REPO_ROOT}" "${root_abs}")
    file(TO_CMAKE_PATH "${root_relative}" root_relative)
    _mir_json_escape(root_json "${root_relative}")
    _mir_json_escape(namespace_json "${ARG_NAMESPACE}")
    set_property(GLOBAL APPEND PROPERTY MIR_SHADER_ASSET_ROOTS
        "{\n    \"root\": \"${root_json}\",\n    \"namespace\": \"${namespace_json}\"\n  }")
    file(GLOB_RECURSE material_files CONFIGURE_DEPENDS "${root_abs}/*.material.json")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${material_files})
    file(GLOB_RECURSE terrain_material_files CONFIGURE_DEPENDS "${root_abs}/*.terrainmaterial.json")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${terrain_material_files})

    set(forward "${MIR_REPO_ROOT}/assets/shaders/SurfaceForward.slang")
    set(rt "${MIR_REPO_ROOT}/assets/shaders/SurfaceRt.slang")
    set(terrain_forward "${MIR_REPO_ROOT}/assets/shaders/SurfaceTerrainForward.slang")
    set(terrain_rt "${MIR_REPO_ROOT}/assets/shaders/SurfaceTerrainRt.slang")
    set(layout_pass "${MIR_REPO_ROOT}/assets/shaders/SurfaceLayout.slang")
    _engine_scan_shader_includes("${forward}" forward_deps)
    _engine_scan_shader_includes("${rt}" mesh_rt_deps)
    _engine_scan_shader_includes("${terrain_forward}" terrain_forward_deps)
    _engine_scan_shader_includes("${terrain_rt}" terrain_rt_deps)
    _engine_scan_shader_includes("${layout_pass}" layout_deps)

    set(gen_dir "${CMAKE_BINARY_DIR}/shader_build")
    set(surface_outputs "")
    set(surface_reflections "")
    get_property(compiled_keys GLOBAL PROPERTY MIR_SURFACE_COMPILED_KEYS)
    get_property(compiled_stems GLOBAL PROPERTY MIR_SURFACE_COMPILED_STEMS)

    foreach(material IN LISTS material_files)
        file(READ "${material}" material_json)
        string(JSON version ERROR_VARIABLE json_error GET "${material_json}" version)
        if(json_error)
            message(FATAL_ERROR "${material}: not valid material JSON (${json_error})")
        endif()
        if(NOT version EQUAL 1)
            message(FATAL_ERROR "${material}: unsupported material version ${version}")
        endif()

        string(JSON shader_file ERROR_VARIABLE json_error GET "${material_json}" shader file)
        if(json_error)
            message(FATAL_ERROR "${material}: shader.file is required")
        endif()
        string(JSON shader_type ERROR_VARIABLE json_error GET "${material_json}" shader type)
        if(json_error OR NOT shader_type)
            message(FATAL_ERROR "${material}: shader.type is required")
        endif()
        file(TO_CMAKE_PATH "${shader_file}" shader_file)

        string(JSON frontend_count ERROR_VARIABLE json_error LENGTH "${material_json}" frontends)
        if(json_error OR frontend_count EQUAL 0)
            message(FATAL_ERROR "${material}: frontends must be a non-empty array")
        endif()

        set(frontends "")
        math(EXPR frontend_last "${frontend_count} - 1")
        foreach(index RANGE ${frontend_last})
            string(JSON frontend GET "${material_json}" frontends ${index})
            if(frontend STREQUAL "mesh" OR frontend STREQUAL "terrain")
                list(APPEND frontends "${frontend}")
            else()
                message(FATAL_ERROR "${material}: unknown frontend '${frontend}'")
            endif()
        endforeach()

        if(IS_ABSOLUTE "${shader_file}" OR shader_file MATCHES "(^|/)\\.\\.(/|$)")
            message(FATAL_ERROR "${material}: shader.file must stay under an approved assets root")
        endif()

        set(approved_roots "${root_abs}")
        foreach(approved IN LISTS ARG_APPROVED_ROOTS)
            get_filename_component(approved_abs "${approved}" ABSOLUTE BASE_DIR "${MIR_REPO_ROOT}")
            list(APPEND approved_roots "${approved_abs}")
        endforeach()

        set(shader_abs "")
        set(shader_rel "")
        foreach(approved IN LISTS approved_roots)
            set(candidate "${approved}/${shader_file}")
            if(EXISTS "${candidate}")
                file(RELATIVE_PATH candidate_rel "${MIR_REPO_ROOT}" "${candidate}")
                file(TO_CMAKE_PATH "${candidate_rel}" candidate_rel)
                if(NOT candidate_rel MATCHES "^\\.\\.")
                    set(shader_abs "${candidate}")
                    set(shader_rel "${candidate_rel}")
                    break()
                endif()
            endif()
        endforeach()
        if(NOT shader_abs)
            message(FATAL_ERROR "${material}: shader file '${shader_file}' was not found under an approved assets root")
        endif()

        file(RELATIVE_PATH material_rel "${MIR_REPO_ROOT}" "${material}")
        file(TO_CMAKE_PATH "${material_rel}" material_rel)
        string(JSON parameters ERROR_VARIABLE parameters_error GET "${material_json}" parameters)
        if(parameters_error)
            set(parameters "{}")
        endif()
        string(JSON parameters_type ERROR_VARIABLE parameters_type_error TYPE "${parameters}")
        if(parameters_type_error OR NOT parameters_type STREQUAL "OBJECT")
            message(FATAL_ERROR "${material}: parameters must be an object")
        endif()

        foreach(frontend IN LISTS frontends)
            _mir_surface_identity(stem key "${ARG_NAMESPACE}" "${shader_type}" "${shader_rel}" "${frontend}")
            list(FIND compiled_keys "${key}" compiled_index)
            if(NOT compiled_index EQUAL -1)
                list(GET compiled_stems ${compiled_index} stem)
            endif()
            set(reflection "${gen_dir}/${stem}.layout.spv.reflection.json")
            get_filename_component(reflection_name "${reflection}" NAME)

            _mir_json_escape(stem_json "${stem}")
            _mir_json_escape(type_json "${shader_type}")
            _mir_json_escape(asset_json "${material_rel}")
            _mir_json_escape(shader_json "${shader_rel}")
            _mir_json_escape(frontend_json "${frontend}")
            _mir_json_escape(reflection_json "${reflection_name}")
            string(CONCAT catalog_entry
                "{\n      \"stem\": \"${stem_json}\",\n      \"type\": \"${type_json}\",\n"
                "      \"frontend\": \"${frontend_json}\",\n      \"asset\": \"${asset_json}\",\n"
                "      \"shader\": \"${shader_json}\",\n      \"parameters\": ${parameters},\n"
                "      \"reflection\": \"${reflection_json}\"\n    }")
            set_property(GLOBAL APPEND PROPERTY MIR_SURFACE_CATALOG_ENTRIES "${catalog_entry}")

            if(key IN_LIST compiled_keys)
                foreach(entry IN ITEMS vertexMain fragmentMain surface.fragmentMain closestHitSurface layout)
                    list(APPEND surface_outputs "${gen_dir}/${stem}.${entry}.spv")
                    list(APPEND surface_reflections "${gen_dir}/${stem}.${entry}.spv.reflection.json")
                endforeach()
                continue()
            endif()
            set_property(GLOBAL APPEND PROPERTY MIR_SURFACE_COMPILED_KEYS "${key}")
            set_property(GLOBAL APPEND PROPERTY MIR_SURFACE_COMPILED_STEMS "${stem}")
            list(APPEND compiled_keys "${key}")
            list(APPEND compiled_stems "${stem}")

            if(frontend STREQUAL "terrain")
                set(front_pass "${terrain_forward}")
                set(front_deps ${terrain_forward_deps})
                set(rt_pass "${terrain_rt}")
                set(rt_deps ${terrain_rt_deps})
                _mir_register_terrain_frontend_template()
            else()
                set(front_pass "${forward}")
                set(front_deps ${forward_deps})
                set(rt_pass "${rt}")
                set(rt_deps ${mesh_rt_deps})
                _mir_register_mesh_frontend_template()
            endif()

            set(outputs "${gen_dir}/${stem}.vertexMain.spv;${gen_dir}/${stem}.fragmentMain.spv;${gen_dir}/${stem}.surface.fragmentMain.spv;${gen_dir}/${stem}.closestHitSurface.spv;${gen_dir}/${stem}.layout.spv")
            if(MIR_WITH_RTXGI)
                list(APPEND outputs "${gen_dir}/${stem}.gi.closestHitSurface.spv" "${gen_dir}/${stem}.probe.closestHitSurface.spv")
            endif()
            foreach(output IN LISTS outputs)
                set(output_reflection "${output}.reflection.json")
                get_filename_component(output_name "${output}" NAME)
                if(output_name MATCHES "\\.vertexMain\\.spv$")
                    set(entry vertexMain)
                    set(stage vertex)
                    set(sources "${front_pass}")
                    set(deps ${front_deps} "${shader_abs}")
                    set(specialize "")
                elseif(output_name MATCHES "\\.fragmentMain\\.spv$")
                    set(entry fragmentMain)
                    set(stage fragment)
                    set(sources "${front_pass};${shader_abs}")
                    set(deps ${front_deps} "${shader_abs}")
                    set(specialize "${shader_type}")
                elseif(output_name MATCHES "\\.closestHitSurface\\.spv$")
                    set(entry closestHitSurface)
                    set(stage closesthit)
                    set(sources "${rt_pass};${shader_abs}")
                    set(deps ${rt_deps} "${shader_abs}")
                    set(specialize "${shader_type}")
                else()
                    set(entry layoutMain)
                    set(stage compute)
                    set(sources "${layout_pass};${shader_abs}")
                    set(deps ${layout_deps} "${shader_abs}")
                    set(specialize "${shader_type}")
                endif()

                set(depfile "${output}.d")
                set(extra_flags -fvk-use-gl-layout)
                if(output_name STREQUAL "${stem}.surface.fragmentMain.spv")
                    list(APPEND extra_flags -DMIR_SSGI_SURFACE)
                endif()
                if(output_name MATCHES "\\.(gi|probe)\\.closestHitSurface\\.spv$")
                    list(APPEND extra_flags -DMIR_DDGI)
                    if(output_name MATCHES "\\.probe\\.")
                        list(APPEND extra_flags -DMIR_DDGI_PROBE)
                    endif()
                endif()
                set(specialize_args "")
                set(arguments "${SLANGC_EXECUTABLE}" ${sources} ${MIR_SHADER_COMMON_ARGS} ${extra_flags}
                    -entry "${entry}" -stage "${stage}")
                if(specialize)
                    set(specialize_args -specialize "${specialize}")
                    list(APPEND arguments ${specialize_args})
                endif()
                list(APPEND arguments -reflection-json "${output_reflection}" -depfile "${depfile}" -o "${output}")

                _mir_compile_command(compile_command "${output}" ${arguments})

                add_custom_command(
                    OUTPUT "${output}"
                    COMMAND "${CMAKE_COMMAND}" -E make_directory "${gen_dir}"
                    COMMAND ${compile_command}
                    DEPENDS ${deps}
                    DEPFILE "${depfile}"
                    BYPRODUCTS "${output_reflection}"
                    COMMENT "slangc ${stem}.slang [${entry}/${stage}]"
                    VERBATIM
                    COMMAND_EXPAND_LISTS)
                set_property(GLOBAL APPEND PROPERTY MIR_SHADER_GENERATED_OUTPUTS "${output}")
                _mir_manifest_add_output(
                    OUTPUT "${output}"
                    SOURCES ${sources}
                    ENTRY "${entry}"
                    STAGE "${stage}"
                    TYPE surface
                    SPECIALIZE "${specialize}"
                    REFLECTION "${output_reflection}"
                    DEPFILE "${depfile}"
                    FRONTEND "${frontend}"
                    ARGUMENTS ${arguments})

                list(APPEND surface_outputs "${output}")
                list(APPEND surface_reflections "${output_reflection}")
            endforeach()
        endforeach()
    endforeach()

    # A layered material names existing single materials as layers. It compiles the
    # same frontend outputs as a single material, specialized on the layer types.
    set(approved_roots "${root_abs}")
    foreach(approved IN LISTS ARG_APPROVED_ROOTS)
        get_filename_component(approved_abs "${approved}" ABSOLUTE BASE_DIR "${MIR_REPO_ROOT}")
        list(APPEND approved_roots "${approved_abs}")
    endforeach()
    foreach(layered IN LISTS terrain_material_files)
        file(READ "${layered}" layered_json)
        string(JSON layered_version ERROR_VARIABLE json_error GET "${layered_json}" version)
        if(json_error OR NOT layered_version EQUAL 1)
            message(FATAL_ERROR "${layered}: unsupported layered material version")
        endif()
        string(JSON layer_count ERROR_VARIABLE json_error LENGTH "${layered_json}" layers)
        if(json_error OR layer_count LESS 2 OR layer_count GREATER 4)
            message(FATAL_ERROR "${layered}: layers must hold between two and four materials")
        endif()

        string(JSON blend ERROR_VARIABLE json_error GET "${layered_json}" blend)
        if(json_error)
            set(blend weight)
        endif()
        if(blend STREQUAL "weight")
            set(blend_mode 0)
        elseif(blend STREQUAL "height")
            set(blend_mode 1)
        elseif(blend STREQUAL "alpha")
            set(blend_mode 2)
        else()
            message(FATAL_ERROR "${layered}: unknown blend '${blend}'")
        endif()
        string(JSON contrast ERROR_VARIABLE json_error GET "${layered_json}" heightContrast)
        if(json_error)
            set(contrast 0.1)
        endif()
        if(NOT contrast MATCHES "^-?[0-9]")
            message(FATAL_ERROR "${layered}: heightContrast must be a number")
        endif()
        if(contrast LESS_EQUAL 0)
            message(FATAL_ERROR "${layered}: heightContrast must be positive")
        endif()

        set(child_types "")
        set(child_shaders "")
        set(child_deps "")
        set(catalog_layers "")
        math(EXPR layer_last "${layer_count} - 1")
        foreach(index RANGE ${layer_last})
            string(JSON layer_name GET "${layered_json}" layers ${index} name)
            string(JSON layer_material GET "${layered_json}" layers ${index} material)
            file(TO_CMAKE_PATH "${layer_material}" layer_material)
            if(layer_material MATCHES "\\.terrainmaterial\\.json$")
                message(FATAL_ERROR "${layered}: layer '${layer_name}' names a layered material")
            endif()
            if(NOT layer_material MATCHES "\\.material\\.json$")
                message(FATAL_ERROR "${layered}: layer '${layer_name}' must name a .material.json")
            endif()
            if(IS_ABSOLUTE "${layer_material}" OR layer_material MATCHES "(^|/)\\.\\.(/|$)")
                message(FATAL_ERROR "${layered}: layer '${layer_name}' must stay under an approved assets root")
            endif()

            set(child_abs "")
            foreach(approved IN LISTS approved_roots)
                if(EXISTS "${approved}/${layer_material}")
                    set(child_abs "${approved}/${layer_material}")
                    break()
                endif()
            endforeach()
            if(NOT child_abs)
                message(FATAL_ERROR "${layered}: layer '${layer_name}' material '${layer_material}' was not found")
            endif()

            file(READ "${child_abs}" child_json)
            string(JSON child_version ERROR_VARIABLE json_error GET "${child_json}" version)
            if(json_error OR NOT child_version EQUAL 1)
                message(FATAL_ERROR "${child_abs}: unsupported material version")
            endif()
            string(JSON child_shader_file ERROR_VARIABLE json_error GET "${child_json}" shader file)
            if(json_error)
                message(FATAL_ERROR "${child_abs}: shader.file is required")
            endif()
            string(JSON child_shader_type ERROR_VARIABLE json_error GET "${child_json}" shader type)
            if(json_error OR NOT child_shader_type)
                message(FATAL_ERROR "${child_abs}: shader.type is required")
            endif()
            string(JSON child_frontend_count ERROR_VARIABLE json_error LENGTH "${child_json}" frontends)
            if(json_error)
                message(FATAL_ERROR "${child_abs}: frontends must be an array")
            endif()
            set(child_has_terrain FALSE)
            math(EXPR child_frontend_last "${child_frontend_count} - 1")
            foreach(child_index RANGE ${child_frontend_last})
                string(JSON child_frontend GET "${child_json}" frontends ${child_index})
                if(child_frontend STREQUAL "terrain")
                    set(child_has_terrain TRUE)
                endif()
            endforeach()
            if(NOT child_has_terrain)
                message(FATAL_ERROR "${child_abs}: layer '${layer_name}' must declare the terrain frontend")
            endif()

            file(TO_CMAKE_PATH "${child_shader_file}" child_shader_file)
            if(IS_ABSOLUTE "${child_shader_file}" OR child_shader_file MATCHES "(^|/)\\.\\.(/|$)")
                message(FATAL_ERROR "${child_abs}: shader.file must stay under an approved assets root")
            endif()
            set(child_shader_abs "")
            foreach(approved IN LISTS approved_roots)
                if(EXISTS "${approved}/${child_shader_file}")
                    set(child_shader_abs "${approved}/${child_shader_file}")
                    break()
                endif()
            endforeach()
            if(NOT child_shader_abs)
                message(FATAL_ERROR "${child_abs}: shader file '${child_shader_file}' was not found")
            endif()
            list(APPEND child_types "${child_shader_type}")
            if(NOT child_shader_abs IN_LIST child_shaders)
                list(APPEND child_shaders "${child_shader_abs}")
                _engine_scan_shader_includes("${child_shader_abs}" shader_deps)
                list(APPEND child_deps ${shader_deps})
            endif()

            file(RELATIVE_PATH child_rel "${MIR_REPO_ROOT}" "${child_abs}")
            file(TO_CMAKE_PATH "${child_rel}" child_rel)
            file(RELATIVE_PATH child_shader_rel "${MIR_REPO_ROOT}" "${child_shader_abs}")
            file(TO_CMAKE_PATH "${child_shader_rel}" child_shader_rel)
            _mir_json_escape(layer_name_json "${layer_name}")
            _mir_json_escape(child_rel_json "${child_rel}")
            _mir_json_escape(child_shader_type_json "${child_shader_type}")
            _mir_json_escape(child_shader_rel_json "${child_shader_rel}")
            string(CONCAT catalog_layer
                "{ \"name\": \"${layer_name_json}\", \"asset\": \"${child_rel_json}\", \"type\": \"${child_shader_type_json}\", \"shader\": \"${child_shader_rel_json}\" }")
            list(APPEND catalog_layers "${catalog_layer}")
        endforeach()

        string(JOIN ", " child_type_args ${child_types})
        set(specialize "Layered${layer_count}<${child_type_args}>")
        file(RELATIVE_PATH layered_rel "${MIR_REPO_ROOT}" "${layered}")
        file(TO_CMAKE_PATH "${layered_rel}" layered_rel)

        _mir_surface_identity(stem key "${ARG_NAMESPACE}" "${specialize}" "${layered_rel}" "terrain")
        list(FIND compiled_keys "${key}" compiled_index)
        if(NOT compiled_index EQUAL -1)
            list(GET compiled_stems ${compiled_index} stem)
        endif()
        set(reflection "${gen_dir}/${stem}.layout.spv.reflection.json")
        get_filename_component(reflection_name "${reflection}" NAME)

        string(JOIN ", " catalog_layers_json ${catalog_layers})
        _mir_json_escape(stem_json "${stem}")
        _mir_json_escape(type_json "${specialize}")
        _mir_json_escape(layered_json_path "${layered_rel}")
        _mir_json_escape(reflection_json "${reflection_name}")
        string(CONCAT layered_parameters
            "{ \"blendMode\": ${blend_mode}, \"heightContrast\": ${contrast}")
        foreach(index RANGE ${layer_last})
            string(CONCAT layered_parameters "${layered_parameters}, \"layerOffset${index}\": 0")
        endforeach()
        string(CONCAT layered_parameters "${layered_parameters} }")
        string(CONCAT catalog_entry
            "{\n      \"stem\": \"${stem_json}\",\n      \"type\": \"${type_json}\",\n"
            "      \"frontend\": \"terrain\",\n      \"asset\": \"${layered_json_path}\",\n"
            "      \"shader\": \"${layered_json_path}\",\n      \"parameters\": ${layered_parameters},\n"
            "      \"layers\": [${catalog_layers_json}],\n"
            "      \"reflection\": \"${reflection_json}\"\n    }")
        set_property(GLOBAL APPEND PROPERTY MIR_SURFACE_CATALOG_ENTRIES "${catalog_entry}")

        if(key IN_LIST compiled_keys)
            foreach(entry IN ITEMS vertexMain fragmentMain surface.fragmentMain closestHitSurface layout)
                list(APPEND surface_outputs "${gen_dir}/${stem}.${entry}.spv")
                list(APPEND surface_reflections "${gen_dir}/${stem}.${entry}.spv.reflection.json")
            endforeach()
            continue()
        endif()
        set_property(GLOBAL APPEND PROPERTY MIR_SURFACE_COMPILED_KEYS "${key}")
        set_property(GLOBAL APPEND PROPERTY MIR_SURFACE_COMPILED_STEMS "${stem}")
        list(APPEND compiled_keys "${key}")
        list(APPEND compiled_stems "${stem}")

        _mir_register_terrain_frontend_template()
        set(outputs "${gen_dir}/${stem}.vertexMain.spv;${gen_dir}/${stem}.fragmentMain.spv;${gen_dir}/${stem}.surface.fragmentMain.spv;${gen_dir}/${stem}.closestHitSurface.spv;${gen_dir}/${stem}.layout.spv")
        if(MIR_WITH_RTXGI)
            list(APPEND outputs "${gen_dir}/${stem}.gi.closestHitSurface.spv" "${gen_dir}/${stem}.probe.closestHitSurface.spv")
        endif()
        foreach(output IN LISTS outputs)
            set(output_reflection "${output}.reflection.json")
            get_filename_component(output_name "${output}" NAME)
            if(output_name MATCHES "\\.vertexMain\\.spv$")
                set(entry vertexMain)
                set(stage vertex)
                set(sources "${terrain_forward}")
                set(deps ${terrain_forward_deps} "${layered}" ${child_shaders})
                set(specialize_arg "")
            elseif(output_name MATCHES "\\.fragmentMain\\.spv$")
                set(entry fragmentMain)
                set(stage fragment)
                set(sources "${terrain_forward}" ${child_shaders})
                set(deps ${terrain_forward_deps} "${layered}" ${child_deps} ${child_shaders})
                set(specialize_arg "${specialize}")
            elseif(output_name MATCHES "\\.closestHitSurface\\.spv$")
                set(entry closestHitSurface)
                set(stage closesthit)
                set(sources "${terrain_rt}" ${child_shaders})
                set(deps ${terrain_rt_deps} "${layered}" ${child_deps} ${child_shaders})
                set(specialize_arg "${specialize}")
            else()
                set(entry layoutMain)
                set(stage compute)
                set(sources "${layout_pass}" ${child_shaders})
                set(deps ${layout_deps} "${layered}" ${child_deps} ${child_shaders})
                set(specialize_arg "${specialize}")
            endif()

            set(depfile "${output}.d")
            set(extra_flags -fvk-use-gl-layout)
            if(output_name STREQUAL "${stem}.surface.fragmentMain.spv")
                list(APPEND extra_flags -DMIR_SSGI_SURFACE)
            endif()
            if(output_name MATCHES "\\.(gi|probe)\\.closestHitSurface\\.spv$")
                list(APPEND extra_flags -DMIR_DDGI)
                if(output_name MATCHES "\\.probe\\.")
                    list(APPEND extra_flags -DMIR_DDGI_PROBE)
                endif()
            endif()
            set(arguments "${SLANGC_EXECUTABLE}" ${sources} ${MIR_SHADER_COMMON_ARGS} ${extra_flags}
                -entry "${entry}" -stage "${stage}")
            if(specialize_arg)
                list(APPEND arguments -specialize "${specialize_arg}")
            endif()
            list(APPEND arguments -reflection-json "${output_reflection}" -depfile "${depfile}" -o "${output}")

            _mir_compile_command(compile_command "${output}" ${arguments})

            add_custom_command(
                OUTPUT "${output}"
                COMMAND "${CMAKE_COMMAND}" -E make_directory "${gen_dir}"
                COMMAND ${compile_command}
                DEPENDS ${deps}
                DEPFILE "${depfile}"
                BYPRODUCTS "${output_reflection}"
                COMMENT "slangc ${stem}.slang [${entry}/${stage}]"
                VERBATIM
                COMMAND_EXPAND_LISTS)
            set_property(GLOBAL APPEND PROPERTY MIR_SHADER_GENERATED_OUTPUTS "${output}")
            _mir_manifest_add_output(
                OUTPUT "${output}"
                SOURCES ${sources}
                ENTRY "${entry}"
                STAGE "${stage}"
                TYPE surface
                SPECIALIZE "${specialize_arg}"
                REFLECTION "${output_reflection}"
                DEPFILE "${depfile}"
                FRONTEND "terrain"
                ARGUMENTS ${arguments})

            list(APPEND surface_outputs "${output}")
            list(APPEND surface_reflections "${output_reflection}")
        endforeach()
    endforeach()

    if(surface_outputs)
        set(shader_target ${target}_surface_shaders_${ARG_NAMESPACE})
        add_custom_target(${shader_target} DEPENDS ${surface_outputs})
        add_dependencies(${target} ${shader_target})
    endif()
endfunction()
