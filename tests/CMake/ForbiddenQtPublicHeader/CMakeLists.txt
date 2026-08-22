cmake_minimum_required(VERSION 3.24)

project(PBForbiddenQtPublicHeader LANGUAGES NONE)

if(NOT DEFINED PB_MODULE_DIR)
    message(FATAL_ERROR "PB_MODULE_DIR is required")
endif()

list(APPEND CMAKE_MODULE_PATH "${PB_MODULE_DIR}")
include(PBTargetPolicy)

PbValidateCorePublicHeaders("${CMAKE_CURRENT_SOURCE_DIR}")
