# cmake -P script run by engine_write_player_info (cmake/Player.cmake).
# Takes OUT, GIT, SOURCE_DIR, ENGINE_VERSION, CONFIG and one true/false per
# build option, and writes OUT only when its content changes.

set(commit "unknown")
if(GIT)
    execute_process(COMMAND "${GIT}" -C "${SOURCE_DIR}" rev-parse HEAD
        OUTPUT_VARIABLE head
        RESULT_VARIABLE result
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(result EQUAL 0)
        set(commit "${head}")
    endif()
endif()

string(CONCAT content
    "{\n"
    "  \"version\": 1,\n"
    "  \"engineVersion\": \"${ENGINE_VERSION}\",\n"
    "  \"commit\": \"${commit}\",\n"
    "  \"config\": \"${CONFIG}\",\n"
    "  \"editor\": ${MJOLNIR_WITH_EDITOR},\n"
    "  \"largeWorld\": ${MIR_LARGE_WORLD},\n"
    "  \"audio\": ${MIR_WITH_AUDIO},\n"
    "  \"dlss\": ${MIR_WITH_DLSS},\n"
    "  \"rtxgi\": ${MIR_WITH_RTXGI}\n"
    "}\n")

set(existing "")
if(EXISTS "${OUT}")
    file(READ "${OUT}" existing)
endif()
if(NOT existing STREQUAL content)
    file(WRITE "${OUT}" "${content}")
endif()
