cmake_minimum_required(VERSION 3.24)

project(PBForbiddenQtAliasDependency LANGUAGES NONE)

if(NOT DEFINED PB_MODULE_DIR)
    message(FATAL_ERROR "PB_MODULE_DIR is required")
endif()

list(APPEND CMAKE_MODULE_PATH "${PB_MODULE_DIR}")
include(PBTargetPolicy)

add_library(Qt6::Core INTERFACE IMPORTED)
add_library(Qt::Core ALIAS Qt6::Core)
add_library(PBQtWrapper INTERFACE)
target_link_libraries(PBQtWrapper INTERFACE "$<BUILD_INTERFACE:Qt::Core>")
add_library(PBPolicyViolation INTERFACE)
target_link_libraries(PBPolicyViolation INTERFACE PBQtWrapper)

PbValidateTargetBoundary(PBPolicyViolation)
