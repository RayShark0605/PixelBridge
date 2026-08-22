set(pbStaticIsolationSourceDirectory
    "${CMAKE_CURRENT_SOURCE_DIR}/StaticLibraryIsolation")
set(pbStaticIsolationBinaryDirectory
    "${CMAKE_CURRENT_BINARY_DIR}/StaticLibraryIsolation-build")

if(NOT DEFINED blake3_DIR OR blake3_DIR STREQUAL "")
    message(FATAL_ERROR
        "blake3_DIR is required by the PBProtocol consumer fixtures")
endif()

set(pbStaticIsolationConfigureCommand
    "${CMAKE_COMMAND}"
    --fresh
    -S "${pbStaticIsolationSourceDirectory}"
    -B "${pbStaticIsolationBinaryDirectory}"
    -G "${CMAKE_GENERATOR}"
    "-DPB_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
    "-Dblake3_DIR=${blake3_DIR}")

if(CMAKE_GENERATOR_PLATFORM)
    list(APPEND pbStaticIsolationConfigureCommand
        -A "${CMAKE_GENERATOR_PLATFORM}")
endif()

if(CMAKE_GENERATOR_TOOLSET)
    list(APPEND pbStaticIsolationConfigureCommand
        -T "${CMAKE_GENERATOR_TOOLSET}")
endif()

add_test(NAME PBStaticLibraryIsolationConfigure
    COMMAND ${pbStaticIsolationConfigureCommand})
set_tests_properties(PBStaticLibraryIsolationConfigure PROPERTIES
    FIXTURES_SETUP PBStaticLibraryIsolation
    TIMEOUT 120)

add_test(NAME PBStaticLibraryIsolationBuild
    COMMAND "${CMAKE_COMMAND}"
        --build "${pbStaticIsolationBinaryDirectory}"
        --config $<CONFIG>
        --parallel)
set_tests_properties(PBStaticLibraryIsolationBuild PROPERTIES
    FIXTURES_REQUIRED PBStaticLibraryIsolation
    FIXTURES_SETUP PBStaticLibraryIsolationBuilt
    TIMEOUT 120)

add_test(NAME PBStaticLibraryIsolationRun
    COMMAND "${CMAKE_CTEST_COMMAND}"
        --test-dir "${pbStaticIsolationBinaryDirectory}"
        --build-config $<CONFIG>
        --output-on-failure)
set_tests_properties(PBStaticLibraryIsolationRun PROPERTIES
    FIXTURES_REQUIRED PBStaticLibraryIsolationBuilt
    TIMEOUT 60)

add_test(NAME PBManifestDependencyBoundary
    COMMAND "${CMAKE_COMMAND}"
        "-DPB_MANIFEST_PATH=${PROJECT_SOURCE_DIR}/vcpkg.json"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyManifest.cmake")

add_test(NAME PBBuildOptionValidation
    COMMAND "${CMAKE_COMMAND}"
        "-DPB_MODULE_DIR=${PROJECT_SOURCE_DIR}/cmake"
        "-DPB_PROBE_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/BuildOptionValidationProbe.cmake"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyBuildOptionMatrix.cmake")
set_tests_properties(PBBuildOptionValidation PROPERTIES TIMEOUT 30)

function(PbAddForbiddenQtConfigureTest testName fixtureName expectedErrorRegex)
    add_test(NAME ${testName}
        COMMAND "${CMAKE_COMMAND}"
            "-DPB_FIXTURE_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/${fixtureName}"
            "-DPB_FIXTURE_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/${fixtureName}-build"
            "-DPB_MODULE_DIR=${PROJECT_SOURCE_DIR}/cmake"
            "-DPB_GENERATOR=${CMAKE_GENERATOR}"
            "-DPB_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
            "-DPB_EXPECTED_ERROR_REGEX=${expectedErrorRegex}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/ExpectForbiddenQtConfigure.cmake")
    set_tests_properties(${testName} PROPERTIES TIMEOUT 60)
endfunction()

PbAddForbiddenQtConfigureTest(
    PBQtBoundaryRejectsCoreDependency
    ForbiddenQtDependency
    "Core target PBPolicyViolation has a forbidden Qt dependency")
PbAddForbiddenQtConfigureTest(
    PBQtBoundaryRejectsAliasedTransitiveDependency
    ForbiddenQtAliasDependency
    "PBQtWrapper:.*Qt::Core")
PbAddForbiddenQtConfigureTest(
    PBQtBoundaryRejectsLowercasePublicHeader
    ForbiddenQtPublicHeader
    "includes Qt:.*qobject\\.h")

set(pbSubprojectSourceDirectory
    "${CMAKE_CURRENT_SOURCE_DIR}/SubprojectDefault")
set(pbSubprojectBinaryDirectory
    "${CMAKE_CURRENT_BINARY_DIR}/SubprojectDefault-build")
set(pbSubprojectConfigureCommand
    "${CMAKE_COMMAND}"
    --fresh
    -S "${pbSubprojectSourceDirectory}"
    -B "${pbSubprojectBinaryDirectory}"
    -G "${CMAKE_GENERATOR}"
    "-DPB_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
    "-Dblake3_DIR=${blake3_DIR}")

if(CMAKE_GENERATOR_PLATFORM)
    list(APPEND pbSubprojectConfigureCommand
        -A "${CMAKE_GENERATOR_PLATFORM}")
endif()

if(CMAKE_GENERATOR_TOOLSET)
    list(APPEND pbSubprojectConfigureCommand
        -T "${CMAKE_GENERATOR_TOOLSET}")
endif()

add_test(NAME PBSubprojectDefaultConfigure
    COMMAND ${pbSubprojectConfigureCommand})
set_tests_properties(PBSubprojectDefaultConfigure PROPERTIES
    FIXTURES_SETUP PBSubprojectDefault
    TIMEOUT 120)

add_test(NAME PBSubprojectDefaultBuild
    COMMAND "${CMAKE_COMMAND}"
        --build "${pbSubprojectBinaryDirectory}"
        --config $<CONFIG>
        --parallel)
set_tests_properties(PBSubprojectDefaultBuild PROPERTIES
    FIXTURES_REQUIRED PBSubprojectDefault
    FIXTURES_SETUP PBSubprojectDefaultBuilt
    TIMEOUT 120)

add_test(NAME PBSubprojectDefaultRun
    COMMAND "${CMAKE_CTEST_COMMAND}"
        --test-dir "${pbSubprojectBinaryDirectory}"
        --build-config $<CONFIG>
        --output-on-failure)
set_tests_properties(PBSubprojectDefaultRun PROPERTIES
    FIXTURES_REQUIRED PBSubprojectDefaultBuilt
    TIMEOUT 60)

unset(pbStaticIsolationBinaryDirectory)
unset(pbStaticIsolationConfigureCommand)
unset(pbStaticIsolationSourceDirectory)
unset(pbSubprojectBinaryDirectory)
unset(pbSubprojectConfigureCommand)
unset(pbSubprojectSourceDirectory)
