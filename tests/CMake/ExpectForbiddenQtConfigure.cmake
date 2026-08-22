foreach(requiredVariable IN ITEMS
        PB_FIXTURE_SOURCE_DIR
        PB_FIXTURE_BINARY_DIR
        PB_MODULE_DIR
        PB_GENERATOR)
    if(NOT DEFINED ${requiredVariable} OR "${${requiredVariable}}" STREQUAL "")
        message(FATAL_ERROR "${requiredVariable} is required")
    endif()
endforeach()

if(NOT DEFINED PB_EXPECTED_ERROR_REGEX OR PB_EXPECTED_ERROR_REGEX STREQUAL "")
    set(PB_EXPECTED_ERROR_REGEX "forbidden Qt dependency")
endif()

set(configureCommand
    "${CMAKE_COMMAND}"
    --fresh
    -S "${PB_FIXTURE_SOURCE_DIR}"
    -B "${PB_FIXTURE_BINARY_DIR}"
    -G "${PB_GENERATOR}"
    "-DPB_MODULE_DIR=${PB_MODULE_DIR}")

if(DEFINED PB_GENERATOR_PLATFORM AND NOT PB_GENERATOR_PLATFORM STREQUAL "")
    list(APPEND configureCommand -A "${PB_GENERATOR_PLATFORM}")
endif()

execute_process(
    COMMAND ${configureCommand}
    RESULT_VARIABLE configureResult
    OUTPUT_VARIABLE configureStdout
    ERROR_VARIABLE configureStderr)

if(configureResult STREQUAL "0")
    message(FATAL_ERROR
        "The forbidden Qt dependency fixture configured successfully")
endif()

set(configureOutput "${configureStdout}\n${configureStderr}")
if(NOT configureOutput MATCHES "${PB_EXPECTED_ERROR_REGEX}")
    message(FATAL_ERROR
        "The fixture failed for an unexpected reason:\n${configureOutput}")
endif()
