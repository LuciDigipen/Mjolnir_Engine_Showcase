# cmake -P script for the smoke.build.<game> tests: starts the exe that a
# MjolnirBuild --result file (RESULT) names, with no --game and from its own
# folder as a double-click does, and fails unless it exits 0.

file(READ "${RESULT}" result)
string(JSON exe ERROR_VARIABLE error GET "${result}" executable)
if(error)
    message(FATAL_ERROR "${RESULT} names no executable: ${error}")
endif()
file(TO_CMAKE_PATH "${exe}" exe)
get_filename_component(dir "${exe}" DIRECTORY)

execute_process(COMMAND "${exe}" --non-interactive --hidden --frames 120
    WORKING_DIRECTORY "${dir}"
    RESULT_VARIABLE exit)
if(NOT exit EQUAL 0)
    message(FATAL_ERROR "${exe} exited with ${exit}; see ${dir}/logs/engine.log")
endif()
