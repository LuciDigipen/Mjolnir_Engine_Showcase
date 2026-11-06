# cmake -P script for the build.game_build test: installs the game build in BUILD_DIR
# as a game build template in PREFIX, replacing the one an earlier run left.

file(REMOVE_RECURSE "${PREFIX}")
execute_process(COMMAND "${CMAKE_COMMAND}" --install "${BUILD_DIR}" --prefix "${PREFIX}"
    RESULT_VARIABLE exit)
if(NOT exit EQUAL 0)
    message(FATAL_ERROR "cmake --install ${BUILD_DIR} --prefix ${PREFIX} failed (${exit})")
endif()
