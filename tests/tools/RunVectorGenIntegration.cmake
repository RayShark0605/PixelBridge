if(NOT DEFINED PB_VECTOR_GEN OR NOT EXISTS "${PB_VECTOR_GEN}")
    message(FATAL_ERROR "PB_VECTOR_GEN does not name an executable: ${PB_VECTOR_GEN}")
endif()
if(NOT DEFINED PB_FRAME_INSPECTOR OR NOT EXISTS "${PB_FRAME_INSPECTOR}")
    message(FATAL_ERROR "PB_FRAME_INSPECTOR does not name an executable: ${PB_FRAME_INSPECTOR}")
endif()
if(NOT DEFINED PB_SOURCE_ROOT OR NOT IS_DIRECTORY "${PB_SOURCE_ROOT}")
    message(FATAL_ERROR "PB_SOURCE_ROOT does not name the source tree: ${PB_SOURCE_ROOT}")
endif()
if(NOT DEFINED PB_WORK_ROOT OR PB_WORK_ROOT STREQUAL "")
    message(FATAL_ERROR "PB_WORK_ROOT is required")
endif()

function(RunExpect expectedExit)
    execute_process(
        COMMAND ${ARGN}
        RESULT_VARIABLE actualExit
        OUTPUT_VARIABLE standardOutput
        ERROR_VARIABLE standardError)
    if(NOT actualExit MATCHES "^-?[0-9]+$")
        message(FATAL_ERROR
            "process did not start: ${ARGN}\nstdout:\n${standardOutput}\nstderr:\n${standardError}")
    endif()
    if(NOT actualExit EQUAL expectedExit)
        message(FATAL_ERROR
            "unexpected exit ${actualExit}, expected ${expectedExit}: ${ARGN}\nstdout:\n${standardOutput}\nstderr:\n${standardError}")
    endif()
endfunction()

function(CompareBinaryTrees leftRoot rightRoot label)
    file(GLOB_RECURSE leftFiles RELATIVE "${leftRoot}" "${leftRoot}/*.bin")
    file(GLOB_RECURSE rightFiles RELATIVE "${rightRoot}" "${rightRoot}/*.bin")
    list(SORT leftFiles)
    list(SORT rightFiles)
    if(NOT "${leftFiles}" STREQUAL "${rightFiles}")
        message(FATAL_ERROR
            "${label}: artifact names differ\nleft=${leftFiles}\nright=${rightFiles}")
    endif()
    foreach(relativeFile IN LISTS leftFiles)
        set(leftFile "${leftRoot}/${relativeFile}")
        set(rightFile "${rightRoot}/${relativeFile}")
        file(SIZE "${leftFile}" leftSize)
        file(SIZE "${rightFile}" rightSize)
        if(NOT leftSize EQUAL rightSize)
            message(FATAL_ERROR
                "${label}: size mismatch ${relativeFile}: expected=${leftSize} actual=${rightSize}")
        endif()
        file(SHA256 "${leftFile}" leftDigest)
        file(SHA256 "${rightFile}" rightDigest)
        if(NOT leftDigest STREQUAL rightDigest)
            message(FATAL_ERROR
                "${label}: digest mismatch ${relativeFile}: expected=${leftDigest} actual=${rightDigest}")
        endif()
    endforeach()
endfunction()

file(REMOVE_RECURSE "${PB_WORK_ROOT}")
file(MAKE_DIRECTORY "${PB_WORK_ROOT}")

set(goldenFirst "${PB_WORK_ROOT}/golden-first")
set(goldenSecond "${PB_WORK_ROOT}/golden-second")
RunExpect(0 "${PB_VECTOR_GEN}" write-golden "${goldenFirst}")
RunExpect(0 "${PB_VECTOR_GEN}" write-golden "${goldenSecond}")
CompareBinaryTrees("${goldenFirst}" "${goldenSecond}" "golden replay")
foreach(category IN ITEMS protocol ldpc interleave raster)
    CompareBinaryTrees(
        "${PB_SOURCE_ROOT}/tests/golden/${category}"
        "${goldenFirst}/${category}"
        "committed golden ${category}")
endforeach()

set(corpusCommitted "${PB_WORK_ROOT}/corpus-committed")
set(corpusSeedFirst "${PB_WORK_ROOT}/corpus-seed-first")
set(corpusSeedSecond "${PB_WORK_ROOT}/corpus-seed-second")
foreach(category IN ITEMS transport interleave ldpc)
    RunExpect(0 "${PB_VECTOR_GEN}" write-corpus "${corpusCommitted}" --category "${category}")
    CompareBinaryTrees(
        "${PB_SOURCE_ROOT}/fuzz/corpus/${category}"
        "${corpusCommitted}/${category}"
        "committed corpus ${category}")
    RunExpect(0 "${PB_VECTOR_GEN}" write-corpus "${corpusSeedFirst}" --category "${category}" --seed 12648430)
    RunExpect(0 "${PB_VECTOR_GEN}" write-corpus "${corpusSeedSecond}" --category "${category}" --seed 12648430)
    CompareBinaryTrees(
        "${corpusSeedFirst}/${category}"
        "${corpusSeedSecond}/${category}"
        "seeded corpus ${category}")
endforeach()

set(rawFrame "${PB_WORK_ROOT}/g1-transport.pbrw")
set(pngFrame "${PB_WORK_ROOT}/g1-transport.png")
RunExpect(0 "${PB_VECTOR_GEN}" write-frame "${rawFrame}" --vector g1-transport --format pbrw)
RunExpect(0 "${PB_VECTOR_GEN}" write-frame "${pngFrame}" --format png --vector g1-transport)
RunExpect(0 "${PB_VECTOR_GEN}" write-frame "${rawFrame}" --vector g1-transport --format pbrw)
RunExpect(0 "${PB_FRAME_INSPECTOR}" --recovery "${rawFrame}")
RunExpect(0 "${PB_FRAME_INSPECTOR}" --recovery "${pngFrame}")

set(conflictRoot "${PB_WORK_ROOT}/conflict")
file(MAKE_DIRECTORY "${conflictRoot}/transport")
set(conflictFile "${conflictRoot}/transport/valid-minimum.bin")
file(WRITE "${conflictFile}" "not-the-vector")
RunExpect(1 "${PB_VECTOR_GEN}" write-corpus "${conflictRoot}" --category transport)
file(READ "${conflictFile}" conflictContents)
if(NOT conflictContents STREQUAL "not-the-vector")
    message(FATAL_ERROR "conflicting target was modified")
endif()

message(STATUS "PBVectorGen deterministic integration completed")
