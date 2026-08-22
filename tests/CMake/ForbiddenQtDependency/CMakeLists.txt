cmake_minimum_required(VERSION 3.24)

project(PBForbiddenQtDependency LANGUAGES NONE)

if(NOT DEFINED PB_MODULE_DIR)
    message(FATAL_ERROR "PB_MODULE_DIR is required")
endif()

list(APPEND CMAKE_MODULE_PATH "${PB_MODULE_DIR}")
include(PBTargetPolicy)

add_library(Qt6::Core INTERFACE IMPORTED)
add_library(PBPolicyViolation INTERFACE)
target_link_libraries(PBPolicyViolation INTERFACE Qt6::Core)

PbValidateTargetBoundary(PBPolicyViolation)
