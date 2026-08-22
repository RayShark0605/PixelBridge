if(NOT DEFINED PB_MANIFEST_PATH OR NOT EXISTS "${PB_MANIFEST_PATH}")
    message(FATAL_ERROR "PB_MANIFEST_PATH must name the PixelBridge manifest")
endif()

file(READ "${PB_MANIFEST_PATH}" manifestJson)

string(JSON baseDependencyCount LENGTH "${manifestJson}" dependencies)
set(blake3DependencyCount 0)
if(baseDependencyCount GREATER 0)
    math(EXPR lastBaseDependencyIndex "${baseDependencyCount} - 1")
    foreach(dependencyIndex RANGE 0 ${lastBaseDependencyIndex})
        string(JSON dependencyType TYPE
            "${manifestJson}" dependencies ${dependencyIndex})
        if(dependencyType STREQUAL "STRING")
            string(JSON dependencyName GET
                "${manifestJson}" dependencies ${dependencyIndex})
        else()
            string(JSON dependencyName GET
                "${manifestJson}" dependencies ${dependencyIndex} name)
        endif()

        if(dependencyName STREQUAL "catch2")
            message(FATAL_ERROR
                "Catch2 must not be an unconditional manifest dependency")
        endif()
        if(dependencyName STREQUAL "blake3")
            math(EXPR blake3DependencyCount
                "${blake3DependencyCount} + 1")
        endif()
    endforeach()
endif()

if(NOT blake3DependencyCount EQUAL 1)
    message(FATAL_ERROR
        "The base manifest must contain exactly one BLAKE3 dependency")
endif()

string(JSON testDependencyCount LENGTH
    "${manifestJson}" features tests dependencies)
if(testDependencyCount LESS 1)
    message(FATAL_ERROR "The tests feature must depend on Catch2")
endif()

set(catch2DependencyCount 0)
math(EXPR lastTestDependencyIndex "${testDependencyCount} - 1")
foreach(dependencyIndex RANGE 0 ${lastTestDependencyIndex})
    string(JSON dependencyName GET
        "${manifestJson}" features tests dependencies ${dependencyIndex} name)
    if(dependencyName STREQUAL "catch2")
        math(EXPR catch2DependencyCount "${catch2DependencyCount} + 1")
    endif()
endforeach()

if(NOT catch2DependencyCount EQUAL 1)
    message(FATAL_ERROR
        "The tests feature must contain exactly one Catch2 dependency")
endif()
