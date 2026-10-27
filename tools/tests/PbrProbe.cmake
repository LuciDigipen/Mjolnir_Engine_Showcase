if(NOT DEFINED SLANGC_EXECUTABLE OR NOT DEFINED SOURCE OR NOT DEFINED OUTPUT)
    message(FATAL_ERROR "PbrProbe.cmake needs SLANGC_EXECUTABLE, SOURCE and OUTPUT")
endif()

execute_process(
    COMMAND "${SLANGC_EXECUTABLE}" "${SOURCE}"
            -target spirv -profile spirv_1_5 -entry probeMain -stage compute
            -fvk-use-entrypoint-name -matrix-layout-column-major -O0
            -o "${OUTPUT}"
    RESULT_VARIABLE compile_result)
if(NOT compile_result EQUAL 0)
    message(FATAL_ERROR "PBR probe failed to compile: ${compile_result}")
endif()

if(DEFINED VULKAN_SDK AND EXISTS "${VULKAN_SDK}/Bin/spirv-val.exe")
    execute_process(
        COMMAND "${VULKAN_SDK}/Bin/spirv-val.exe" --target-env vulkan1.4 "${OUTPUT}"
        RESULT_VARIABLE validate_result)
    if(NOT validate_result EQUAL 0)
        message(FATAL_ERROR "PBR probe failed spirv-val: ${validate_result}")
    endif()
endif()

message(STATUS "PBR probe compiled and validated")
