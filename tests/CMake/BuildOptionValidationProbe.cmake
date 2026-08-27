if(NOT DEFINED PB_MODULE_DIR)
    message(FATAL_ERROR "PB_MODULE_DIR is required")
endif()

include("${PB_MODULE_DIR}/PBBuildOptionValidation.cmake")
if(DEFINED PB_PROBE_WINDOWS)
    set(WIN32 "${PB_PROBE_WINDOWS}")
endif()
PbValidateBuildOptions()
