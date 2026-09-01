# Step 01 of the low-FPS RemoteVisual route freezes the annotated tag "phase1-gate-pass" as the
# boundary between the historical Phase-1 Gate evidence and every later low-FPS change. The route
# document forbids amending, moving or recreating that tag, so its identity has to be re-checked
# automatically instead of relying on a human running git during each later review. This script is
# the single frozen site: CTest runs it on every suite run, and the portable-package tool runs it
# before recording tag identity, so a moved tag can no longer be packaged silently.
#
# Inputs:
#   PB_REPOSITORY_ROOT    mandatory; git work tree to inspect
#   PB_GIT_EXECUTABLE     optional; defaults to "git" from PATH
# The identifiers below are the frozen baseline. They are deliberately not caller-supplied: an
# override would let any future caller "pass" the check by passing along whatever the tag points at.
set(PB_PHASE1_GATE_TAG_NAME_FROZEN "phase1-gate-pass")
set(PB_PHASE1_GATE_TAG_OBJECT_FROZEN "fde56c4c4e7124e8ffe29a0dcb619f8236781ebb")
set(PB_PHASE1_GATE_COMMIT_FROZEN "80699813b595bcf6db64047b50d31056872e33e1")

foreach(pbOverrideName IN ITEMS PB_PHASE1_GATE_TAG_NAME PB_PHASE1_GATE_TAG_OBJECT PB_PHASE1_GATE_COMMIT)
    if(DEFINED ${pbOverrideName})
        message(FATAL_ERROR "${pbOverrideName} must not be supplied; the Step 01 baseline is frozen inside this script")
    endif()
endforeach()

if(NOT DEFINED PB_REPOSITORY_ROOT OR NOT IS_DIRECTORY "${PB_REPOSITORY_ROOT}")
    message(FATAL_ERROR "PB_REPOSITORY_ROOT is missing or not a directory")
endif()

set(pbGit "git")
if(DEFINED PB_GIT_EXECUTABLE AND NOT PB_GIT_EXECUTABLE STREQUAL "")
    set(pbGit "${PB_GIT_EXECUTABLE}")
endif()

function(pb_validate_pin)
    cmake_parse_arguments(ARG "" "LABEL;VALUE" "" ${ARGN})
    string(LENGTH "${ARG_VALUE}" pbPinLength)
    string(REGEX REPLACE "[0-9a-f]" "" pbPinRest "${ARG_VALUE}")
    if(NOT pbPinLength EQUAL 40 OR NOT pbPinRest STREQUAL "")
        message(FATAL_ERROR "${ARG_LABEL} is not a 40-character lowercase hex identifier: '${ARG_VALUE}'")
    endif()
endfunction()

# A blank or malformed constant would match nothing and silently disable the check, so the frozen
# baseline is validated before any repository state is consulted.
pb_validate_pin(LABEL "frozen pin PB_PHASE1_GATE_TAG_OBJECT_FROZEN" VALUE "${PB_PHASE1_GATE_TAG_OBJECT_FROZEN}")
pb_validate_pin(LABEL "frozen pin PB_PHASE1_GATE_COMMIT_FROZEN" VALUE "${PB_PHASE1_GATE_COMMIT_FROZEN}")

function(pb_run_git)
    cmake_parse_arguments(ARG "" "" "ARGS" ${ARGN})
    execute_process(
        COMMAND "${pbGit}" "-C" "${PB_REPOSITORY_ROOT}" ${ARG_ARGS}
        OUTPUT_VARIABLE pbStandardOut
        ERROR_VARIABLE pbStandardError
        RESULT_VARIABLE pbResult
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_STRIP_TRAILING_WHITESPACE
        TIMEOUT 60)
    if(NOT pbResult EQUAL 0)
        string(REPLACE ";" " " pbArgumentText "${ARG_ARGS}")
        message(FATAL_ERROR "git ${pbArgumentText} failed (exit ${pbResult}): ${pbStandardError}")
    endif()
    set(pb_git_stdout "${pbStandardOut}" PARENT_SCOPE)
endfunction()

function(pb_require_equal)
    cmake_parse_arguments(ARG "" "LABEL;ACTUAL;EXPECTED" "" ${ARGN})
    if(NOT ARG_ACTUAL STREQUAL ARG_EXPECTED)
        set(pb_require_error "${ARG_LABEL}: actual='${ARG_ACTUAL}' frozen='${ARG_EXPECTED}'" PARENT_SCOPE)
    else()
        set(pb_require_error "" PARENT_SCOPE)
    endif()
endfunction()

# Parses the leading header block of an annotated tag object. A keyword only counts at the start of
# a line, so a signature or message body that merely contains "object <hex>" is never accepted.
function(pb_parse_tag_header)
    cmake_parse_arguments(ARG "" "TEXT;TAG_NAME" "" ${ARGN})
    set(pbHeaderText "${ARG_TEXT}")
    string(REPLACE "\r\n" "\n" pbHeaderText "${pbHeaderText}")
    string(FIND "${pbHeaderText}" "\n\n" pbBlankIndex)
    if(pbBlankIndex LESS 0)
        set(pb_header_error "annotated tag object has no blank line before its message" PARENT_SCOPE)
        return()
    endif()
    string(SUBSTRING "${pbHeaderText}" 0 ${pbBlankIndex} pbHeaderBlock)

    set(pbParsedObject "")
    set(pbParsedType "")
    set(pbParsedTag "")
    if(pbHeaderBlock MATCHES "(^|\n)object ([0-9a-f]+)\n")
        set(pbParsedObject "${CMAKE_MATCH_2}")
    endif()
    if(pbHeaderBlock MATCHES "(^|\n)type ([^\n]*)\n")
        set(pbParsedType "${CMAKE_MATCH_2}")
    endif()
    if(pbHeaderBlock MATCHES "(^|\n)tag ([^\n]*)\n")
        set(pbParsedTag "${CMAKE_MATCH_2}")
    endif()
    if(NOT pbHeaderBlock MATCHES "(^|\n)tagger ")
        set(pb_header_error "tag object has no tagger header; it is not a proper annotated tag" PARENT_SCOPE)
        return()
    endif()
    if(pbParsedObject STREQUAL "")
        set(pb_header_error "tag object has no object header" PARENT_SCOPE)
        return()
    endif()
    if(pbParsedType STREQUAL "")
        set(pb_header_error "tag object has no type header" PARENT_SCOPE)
        return()
    endif()
    if(NOT pbParsedTag STREQUAL ARG_TAG_NAME)
        set(pb_header_error "tag object binds ref '${pbParsedTag}', expected '${ARG_TAG_NAME}'" PARENT_SCOPE)
        return()
    endif()
    set(pb_header_error "" PARENT_SCOPE)
    set(pb_parsed_object "${pbParsedObject}" PARENT_SCOPE)
    set(pb_parsed_type "${pbParsedType}" PARENT_SCOPE)
endfunction()

# Self-tests: without them every repository check below could pass because the parser reported
# nothing at all. Each case names the behaviour that must, or must not, be detected.
pb_parse_tag_header(TEXT "object 1111111111111111111111111111111111111111\ntype commit\ntag self-test\ntagger A <a@example.invalid> 1700000000 +0800\n\nfrozen boundary\n" TAG_NAME "self-test")
if(NOT pb_header_error STREQUAL "" OR NOT pb_parsed_object STREQUAL "1111111111111111111111111111111111111111" OR NOT pb_parsed_type STREQUAL "commit")
    message(FATAL_ERROR "tag header parser rejected a valid header: '${pb_header_error}'")
endif()
pb_parse_tag_header(TEXT "type commit\ntag self-test\ntagger A <a@example.invalid> 1700000000 +0800\n\nno object header\n" TAG_NAME "self-test")
if(pb_header_error STREQUAL "")
    message(FATAL_ERROR "tag header parser accepted a header without an object line")
endif()
pb_parse_tag_header(TEXT "object 1111111111111111111111111111111111111111\ntype commit\nxobject 2222222222222222222222222222222222222222\ntag self-test\ntype tree\ntagger A <a@example.invalid> 1700000000 +0800\n\nunanchored noise\n" TAG_NAME "self-test")
if(NOT pb_header_error STREQUAL "" OR NOT pb_parsed_object STREQUAL "1111111111111111111111111111111111111111" OR NOT pb_parsed_type STREQUAL "commit")
    message(FATAL_ERROR "tag header parser is not line-anchored: '${pb_header_error}' object='${pb_parsed_object}' type='${pb_parsed_type}'")
endif()
pb_parse_tag_header(TEXT "object 1111111111111111111111111111111111111111\ntype commit\ntag other-ref\ntagger A <a@example.invalid> 1700000000 +0800\n\nwrong ref name\n" TAG_NAME "self-test")
if(pb_header_error STREQUAL "")
    message(FATAL_ERROR "tag header parser accepted an object bound to a different ref name")
endif()
pb_parse_tag_header(TEXT "object 1111111111111111111111111111111111111111\ntype commit\ntag self-test\ntagger A <a@example.invalid> 1700000000 +0800\nno blank line here" TAG_NAME "self-test")
if(pb_header_error STREQUAL "")
    message(FATAL_ERROR "tag header parser accepted a tag object without a message body")
endif()
pb_parse_tag_header(TEXT "object 1111111111111111111111111111111111111111\ntype commit\ntag self-test\n\nno tagger line\n" TAG_NAME "self-test")
if(pb_header_error STREQUAL "")
    message(FATAL_ERROR "tag header parser accepted a header without a tagger line")
endif()
pb_require_equal(LABEL "self-test" ACTUAL "0000000000000000000000000000000000000000" EXPECTED "1111111111111111111111111111111111111111")
if(pb_require_error STREQUAL "")
    message(FATAL_ERROR "identity comparison did not detect a mismatch")
endif()
pb_require_equal(LABEL "self-test" ACTUAL "1111111111111111111111111111111111111111" EXPECTED "1111111111111111111111111111111111111111")
if(NOT pb_require_error STREQUAL "")
    message(FATAL_ERROR "identity comparison rejected two equal identifiers")
endif()

# git walks up the directory tree until it finds a repository, so a nested or wrong
# PB_REPOSITORY_ROOT could otherwise verify some other repository's tag and still report success.
# The resolved work tree top level therefore has to be the exact root that was asked about.
pb_run_git(ARGS "rev-parse" "--show-toplevel")
if(pb_git_stdout STREQUAL "")
    message(FATAL_ERROR "PB_REPOSITORY_ROOT '${PB_REPOSITORY_ROOT}' is inside no git work tree (bare repository?)")
endif()
file(REAL_PATH "${pb_git_stdout}" pbResolvedGitTopLevel)
file(REAL_PATH "${PB_REPOSITORY_ROOT}" pbResolvedRepositoryRoot)
# Windows paths are case-insensitive, so both resolved paths are compared after lowercasing.
string(TOLOWER "${pbResolvedGitTopLevel}" pbGitTopLevelKey)
string(TOLOWER "${pbResolvedRepositoryRoot}" pbRepositoryRootKey)
if(NOT pbGitTopLevelKey STREQUAL pbRepositoryRootKey)
    message(FATAL_ERROR
        "PB_REPOSITORY_ROOT '${PB_REPOSITORY_ROOT}' resolves to '${pbResolvedRepositoryRoot}' but git reports work tree top level '${pbResolvedGitTopLevel}'; refusing to verify a different repository")
endif()

# A lightweight tag resolves straight to a commit, so its name could be recreated on another commit
# while still satisfying a plain rev-parse comparison.
pb_run_git(ARGS "cat-file" "-t" "${PB_PHASE1_GATE_TAG_NAME_FROZEN}")
pb_require_equal(LABEL "'${PB_PHASE1_GATE_TAG_NAME_FROZEN}' must stay an annotated tag object" ACTUAL "${pb_git_stdout}" EXPECTED "tag")
if(NOT pb_require_error STREQUAL "")
    message(FATAL_ERROR "${pb_require_error}")
endif()

pb_run_git(ARGS "rev-parse" "${PB_PHASE1_GATE_TAG_NAME_FROZEN}")
pb_require_equal(LABEL "annotated tag object identity" ACTUAL "${pb_git_stdout}" EXPECTED "${PB_PHASE1_GATE_TAG_OBJECT_FROZEN}")
if(NOT pb_require_error STREQUAL "")
    message(FATAL_ERROR "${pb_require_error}")
endif()

pb_run_git(ARGS "rev-parse" "${PB_PHASE1_GATE_TAG_NAME_FROZEN}^{commit}")
pb_require_equal(LABEL "peeled commit identity" ACTUAL "${pb_git_stdout}" EXPECTED "${PB_PHASE1_GATE_COMMIT_FROZEN}")
if(NOT pb_require_error STREQUAL "")
    message(FATAL_ERROR "${pb_require_error}")
endif()

# Cross-check the peeled commit against the tag object itself instead of trusting rev-parse twice.
pb_run_git(ARGS "cat-file" "tag" "${PB_PHASE1_GATE_TAG_NAME_FROZEN}")
pb_parse_tag_header(TEXT "${pb_git_stdout}" TAG_NAME "${PB_PHASE1_GATE_TAG_NAME_FROZEN}")
if(NOT pb_header_error STREQUAL "")
    message(FATAL_ERROR "cannot bind '${PB_PHASE1_GATE_TAG_NAME_FROZEN}' to its tag object: ${pb_header_error}")
endif()
pb_require_equal(LABEL "tag object payload commit" ACTUAL "${pb_parsed_object}" EXPECTED "${PB_PHASE1_GATE_COMMIT_FROZEN}")
if(NOT pb_require_error STREQUAL "")
    message(FATAL_ERROR "${pb_require_error}")
endif()
pb_require_equal(LABEL "tag object payload type" ACTUAL "${pb_parsed_type}" EXPECTED "commit")
if(NOT pb_require_error STREQUAL "")
    message(FATAL_ERROR "${pb_require_error}")
endif()

# Step 01 keeps sealed evidence out of the repository, so build and evidence trees must never become
# tracked sources; otherwise the evidence boundary silently moves into the code base.
execute_process(
    COMMAND "${pbGit}" "-C" "${PB_REPOSITORY_ROOT}" "ls-files" "--" "build" "build-p1_5-evidence"
    OUTPUT_VARIABLE pbTrackedEvidence
    ERROR_VARIABLE pbTrackedError
    RESULT_VARIABLE pbTrackedResult
    OUTPUT_STRIP_TRAILING_WHITESPACE
    TIMEOUT 60)
if(NOT pbTrackedResult EQUAL 0)
    message(FATAL_ERROR "cannot inventory tracked build or evidence paths: ${pbTrackedError}")
endif()
string(STRIP "${pbTrackedEvidence}" pbTrackedEvidence)
if(NOT pbTrackedEvidence STREQUAL "")
    message(FATAL_ERROR "build or sealed-evidence artifacts are tracked in git: ${pbTrackedEvidence}")
endif()

message(STATUS "PHASE1_GATE_TAG_IDENTITY_OK tag=${PB_PHASE1_GATE_TAG_NAME_FROZEN} tagObject=${PB_PHASE1_GATE_TAG_OBJECT_FROZEN} commit=${PB_PHASE1_GATE_COMMIT_FROZEN} trackedEvidence=0")
