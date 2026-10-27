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
set_property(GLOBAL PROPERTY MIR_SURFACE_CATALOG_ENTRIES "")
set_property(GLOBAL PROPERTY MIR_SURFACE_COMPILED_KEYS "")

function(_mir_json_escape out_var value)
    string(REPLACE "\\" "\\\\" escaped "${value}")
    string(REPLACE "\"" "\\\"" escaped "${escaped}")
    string(REPLACE "\n" "\\n" escaped "${escaped}")
    set(${out_var} "${escaped}" PARENT_SCOPE)
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
    get_property(catalog_entries GLOBAL PROPERTY MIR_SURFACE_CATALOG_ENTRIES)
    _mir_json_array(common_json ${MIR_SHADER_COMMON_ARGS})
    _mir_json_escape(slangc_json "${SLANGC_EXECUTABLE}")
    _mir_json_escape(working_json "${CMAKE_SOURCE_DIR}")

    string(JOIN ",\n    " outputs_json ${outputs})
    string(JOIN ",\n    " templates_json ${templates})
    string(JOIN ",\n    " catalog_json ${catalog_entries})

    set(manifest "${CMAKE_BINARY_DIR}/shaders/shader_manifest.json")
    string(CONCAT manifest_json
        "{\n  \"version\": 1,\n  \"slangc\": \"${slangc_json}\",\n"
        "  \"workingDirectory\": \"${working_json}\",\n  \"commonArguments\": ${common_json},\n"
        "  \"outputs\": [\n    ${outputs_json}\n  ],\n"
        "  \"frontendTemplates\": [\n    ${templates_json}\n  ]\n}\n")
    file(WRITE "${manifest}" "${manifest_json}")

    set(catalog "${CMAKE_BINARY_DIR}/shaders/surface_materials.json")
    string(CONCAT catalog_file
        "{\n  \"version\": 1,\n  \"materials\": [\n    ${catalog_json}\n  ]\n}\n")
    file(WRITE "${catalog}" "${catalog_file}")

    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${manifest}" "$<TARGET_FILE_DIR:${target}>/shaders/shader_manifest.json"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${catalog}" "$<TARGET_FILE_DIR:${target}>/shaders/surface_materials.json"
        COMMENT "Copying shader manifest and surface material catalog next to ${target}"
        VERBATIM)
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
            list(APPEND result "${included_abs}")
            list(APPEND pending "${included_abs}")
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
    cmake_parse_arguments(ARG "" "SOURCE" "ENTRIES" ${ARGN})

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

    _engine_scan_shader_includes("${source_abs}" included_deps)

    # No generator expression in this path: add_custom_command(OUTPUT) paths
    # containing a genex cannot be matched against add_custom_target(DEPENDS),
    # so the command silently never runs.
    set(gen_dir "${CMAKE_BINARY_DIR}/shaders")

    set(outputs "")

    foreach(entry IN LISTS ARG_ENTRIES)
        string(REPLACE ":" ";" parts "${entry}")
        list(GET parts 0 entry_name)
        list(GET parts 1 entry_stage)

        set(spv "${gen_dir}/${source_name}.${entry_name}.spv")
        set(reflection "${spv}.reflection.json")
        set(depfile "${spv}.d")

        set(arguments "${SLANGC_EXECUTABLE}" "${source_abs}" ${MIR_SHADER_COMMON_ARGS}
            -entry "${entry_name}" -stage "${entry_stage}"
            -reflection-json "${reflection}" -depfile "${depfile}" -o "${spv}")

        add_custom_command(
            OUTPUT "${spv}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${gen_dir}"
            COMMAND ${arguments}
            # DEPENDS on the source (plus anything it #includes,
            # transitively) gives recompile-on-edit for free.
            DEPENDS "${source_abs}" ${included_deps}
            DEPFILE "${depfile}"
            BYPRODUCTS "${reflection}"
            COMMENT "slangc ${source_name}.slang [${entry_name}/${entry_stage}]"
            VERBATIM
            COMMAND_EXPAND_LISTS)
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

    # The exe resolves shaders relative to its own directory, not the CWD.
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory
                "$<TARGET_FILE_DIR:${target}>/shaders"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                ${outputs} "$<TARGET_FILE_DIR:${target}>/shaders"
        COMMENT "Copying ${source_name} shaders next to ${target}"
        VERBATIM
        COMMAND_EXPAND_LISTS)
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
    _mir_surface_template_registered(registered)
    if(registered)
        return()
    endif()

    set(template "{\n    \"frontend\": \"mesh\",\n    \"placeholders\": [\"{materialSource}\", \"{type}\", \"{stem}\"],\n    \"vertex\": [\"{materialSource}\", \"-entry\", \"vertexMain\", \"-stage\", \"vertex\"],\n    \"fragment\": [\"{materialSource}\", \"-entry\", \"fragmentMain\", \"-stage\", \"fragment\", \"-specialize\", \"{type}\"],\n    \"raytracing\": [\"{materialSource}\", \"-entry\", \"closestHitSurface\", \"-stage\", \"closesthit\", \"-specialize\", \"{type}\"],\n    \"layout\": [\"{materialSource}\", \"-entry\", \"layoutMain\", \"-stage\", \"compute\", \"-specialize\", \"{type}\"]\n  }")
    set_property(GLOBAL APPEND PROPERTY MIR_SHADER_MANIFEST_TEMPLATES "${template}")
endfunction()

# engine_add_surface_materials(<target>
#     NAMESPACE <game-or-default>
#     ROOT <repo-relative assets root>
#     APPROVED_ROOTS <root> [...])
#
# Discovers <ROOT>/materials/*.material.json, deduplicates by
# "<namespace>_<type>", and compiles the mesh raster, RT, vertex and layout
# outputs through the fixed frontend sources. A new material only needs the
# JSON and its type; no wrapper source or CMake edit.
function(engine_add_surface_materials target)
    cmake_parse_arguments(ARG "" "NAMESPACE;ROOT" "APPROVED_ROOTS" ${ARGN})
    if(NOT ARG_NAMESPACE OR NOT ARG_ROOT)
        message(FATAL_ERROR "engine_add_surface_materials: NAMESPACE and ROOT are required")
    endif()

    get_filename_component(root_abs "${ARG_ROOT}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
    file(GLOB_RECURSE material_files CONFIGURE_DEPENDS "${root_abs}/*.material.json")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${material_files})

    set(forward "${CMAKE_SOURCE_DIR}/assets/shaders/SurfaceForward.slang")
    set(rt "${CMAKE_SOURCE_DIR}/assets/shaders/SurfaceRt.slang")
    set(layout_pass "${CMAKE_SOURCE_DIR}/assets/shaders/SurfaceLayout.slang")
    _engine_scan_shader_includes("${forward}" forward_deps)
    _engine_scan_shader_includes("${rt}" rt_deps)
    _engine_scan_shader_includes("${layout_pass}" layout_deps)

    set(gen_dir "${CMAKE_BINARY_DIR}/shaders")
    set(surface_outputs "")
    set(surface_reflections "")
    get_property(compiled_keys GLOBAL PROPERTY MIR_SURFACE_COMPILED_KEYS)

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

        set(has_mesh FALSE)
        math(EXPR frontend_last "${frontend_count} - 1")
        foreach(index RANGE ${frontend_last})
            string(JSON frontend GET "${material_json}" frontends ${index})
            if(frontend STREQUAL "mesh")
                set(has_mesh TRUE)
            elseif(frontend STREQUAL "terrain")
                # Compiled by the terrain frontend in M4.
            else()
                message(FATAL_ERROR "${material}: unknown frontend '${frontend}'")
            endif()
        endforeach()

        if(IS_ABSOLUTE "${shader_file}" OR shader_file MATCHES "(^|/)\\.\\.(/|$)")
            message(FATAL_ERROR "${material}: shader.file must stay under an approved assets root")
        endif()

        set(approved_roots "${root_abs}")
        foreach(approved IN LISTS ARG_APPROVED_ROOTS)
            get_filename_component(approved_abs "${approved}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
            list(APPEND approved_roots "${approved_abs}")
        endforeach()

        set(shader_abs "")
        set(shader_rel "")
        foreach(approved IN LISTS approved_roots)
            set(candidate "${approved}/${shader_file}")
            if(EXISTS "${candidate}")
                file(RELATIVE_PATH candidate_rel "${CMAKE_SOURCE_DIR}" "${candidate}")
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

        file(RELATIVE_PATH material_rel "${CMAKE_SOURCE_DIR}" "${material}")
        file(TO_CMAKE_PATH "${material_rel}" material_rel)
        string(JSON parameters ERROR_VARIABLE parameters_error GET "${material_json}" parameters)
        if(parameters_error)
            set(parameters "{}")
        endif()
        string(JSON parameters_type ERROR_VARIABLE parameters_type_error TYPE "${parameters}")
        if(parameters_type_error OR NOT parameters_type STREQUAL "OBJECT")
            message(FATAL_ERROR "${material}: parameters must be an object")
        endif()

        if(NOT has_mesh)
            message(STATUS "Material ${material_rel}: terrain-only frontend waits for M4; no mesh outputs")
            continue()
        endif()

        _mir_surface_identity(stem key "${ARG_NAMESPACE}" "${shader_type}" "${shader_rel}" mesh)
        set(reflection "${gen_dir}/${stem}.layout.spv.reflection.json")
        get_filename_component(reflection_name "${reflection}" NAME)

        _mir_json_escape(stem_json "${stem}")
        _mir_json_escape(type_json "${shader_type}")
        _mir_json_escape(asset_json "${material_rel}")
        _mir_json_escape(shader_json "${shader_rel}")
        _mir_json_escape(reflection_json "${reflection_name}")
        string(CONCAT catalog_entry
            "{\n      \"stem\": \"${stem_json}\",\n      \"type\": \"${type_json}\",\n"
            "      \"frontend\": \"mesh\",\n      \"asset\": \"${asset_json}\",\n"
            "      \"shader\": \"${shader_json}\",\n      \"parameters\": ${parameters},\n"
            "      \"reflection\": \"${reflection_json}\"\n    }")
        set_property(GLOBAL APPEND PROPERTY MIR_SURFACE_CATALOG_ENTRIES "${catalog_entry}")

        if(key IN_LIST compiled_keys)
            continue()
        endif()
        set_property(GLOBAL APPEND PROPERTY MIR_SURFACE_COMPILED_KEYS "${key}")
        list(APPEND compiled_keys "${key}")

        _mir_register_mesh_frontend_template()

        set(outputs "${gen_dir}/${stem}.vertexMain.spv;${gen_dir}/${stem}.fragmentMain.spv;${gen_dir}/${stem}.closestHitSurface.spv;${gen_dir}/${stem}.layout.spv")
        foreach(output IN LISTS outputs)
            set(output_reflection "${output}.reflection.json")
            get_filename_component(output_name "${output}" NAME)
            if(output_name MATCHES "\\.vertexMain\\.spv$")
                set(entry vertexMain)
                set(stage vertex)
                set(sources "${forward}")
                set(deps ${forward_deps} "${shader_abs}")
                set(specialize "")
                set(frontend mesh)
            elseif(output_name MATCHES "\\.fragmentMain\\.spv$")
                set(entry fragmentMain)
                set(stage fragment)
                set(sources "${forward};${shader_abs}")
                set(deps ${forward_deps} "${shader_abs}")
                set(specialize "${shader_type}")
                set(frontend mesh)
            elseif(output_name MATCHES "\\.closestHitSurface\\.spv$")
                set(entry closestHitSurface)
                set(stage closesthit)
                set(sources "${rt};${shader_abs}")
                set(deps ${rt_deps} "${shader_abs}")
                set(specialize "${shader_type}")
                set(frontend mesh)
            else()
                set(entry layoutMain)
                set(stage compute)
                set(sources "${layout_pass};${shader_abs}")
                set(deps ${layout_deps} "${shader_abs}")
                set(specialize "${shader_type}")
                set(frontend "")
            endif()

            set(depfile "${output}.d")
            set(extra_flags -fvk-use-gl-layout)
            set(specialize_args "")
            set(arguments "${SLANGC_EXECUTABLE}" ${sources} ${MIR_SHADER_COMMON_ARGS} ${extra_flags}
                -entry "${entry}" -stage "${stage}")
            if(specialize)
                set(specialize_args -specialize "${specialize}")
                list(APPEND arguments ${specialize_args})
            endif()
            list(APPEND arguments -reflection-json "${output_reflection}" -depfile "${depfile}" -o "${output}")

            add_custom_command(
                OUTPUT "${output}"
                COMMAND "${CMAKE_COMMAND}" -E make_directory "${gen_dir}"
                COMMAND ${arguments}
                DEPENDS ${deps}
                DEPFILE "${depfile}"
                BYPRODUCTS "${output_reflection}"
                COMMENT "slangc ${stem}.slang [${entry}/${stage}]"
                VERBATIM
                COMMAND_EXPAND_LISTS)
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

    if(surface_outputs)
        set(shader_target ${target}_surface_shaders_${ARG_NAMESPACE})
        add_custom_target(${shader_target} DEPENDS ${surface_outputs})
        add_dependencies(${target} ${shader_target})
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:${target}>/shaders"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${surface_outputs} ${surface_reflections}
                    "$<TARGET_FILE_DIR:${target}>/shaders"
            COMMENT "Copying surface material shaders next to ${target}"
            VERBATIM
            COMMAND_EXPAND_LISTS)
    endif()
endfunction()
