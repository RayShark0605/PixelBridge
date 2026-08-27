if(NOT DEFINED PB_GATE_EXECUTABLE OR NOT DEFINED PB_SCRATCH_ROOT OR
   NOT DEFINED PB_EVIDENCE_PATH)
    message(FATAL_ERROR "Scratch safety Gate requires executable, scratch, and evidence paths")
endif()

if(EXISTS "${PB_SCRATCH_ROOT}")
    message(FATAL_ERROR "Preserve existing scratch safety artifacts: ${PB_SCRATCH_ROOT}")
endif()
set(pbExistingCase "${PB_SCRATCH_ROOT}/file-0b")
set(pbSentinel "${pbExistingCase}/preserve.txt")
set(pbSentinelContent "Previously retained scratch must not be removed or overwritten.\n")
file(MAKE_DIRECTORY "${pbExistingCase}")
file(WRITE "${pbSentinel}" "${pbSentinelContent}")

execute_process(
    COMMAND "${PB_GATE_EXECUTABLE}" --mode fast
        --scratch "${PB_SCRATCH_ROOT}"
        --evidence "${PB_EVIDENCE_PATH}.rejected.jsonl"
    RESULT_VARIABLE pbGateExit
    OUTPUT_VARIABLE pbGateOutput
    ERROR_VARIABLE pbGateError)
file(WRITE "${PB_EVIDENCE_PATH}.log" "${pbGateOutput}${pbGateError}")
if(NOT "${pbGateExit}" STREQUAL "1" OR
   NOT pbGateError MATCHES "case scratch already exists" OR
   NOT EXISTS "${pbSentinel}")
    message(FATAL_ERROR "Gate did not fail closed on existing scratch; see ${PB_EVIDENCE_PATH}.log")
endif()
file(READ "${pbSentinel}" pbActualSentinel)
if(NOT "${pbActualSentinel}" STREQUAL "${pbSentinelContent}" OR
   EXISTS "${pbExistingCase}/source.bin" OR
   EXISTS "${pbExistingCase}/output.part" OR
   EXISTS "${pbExistingCase}/output.bin")
    message(FATAL_ERROR "Rejected Gate mutated pre-existing scratch: ${PB_SCRATCH_ROOT}")
endif()

file(WRITE "${PB_EVIDENCE_PATH}"
    "{\"case\":\"scratch-safety\",\"status\":\"pass\",\"expected_exit\":1,\"existing_scratch_immutable\":true}\n")
file(REMOVE_RECURSE "${PB_SCRATCH_ROOT}")
message(STATUS "Phase 0 scratch safety Gate passed; pre-existing artifacts were preserved")
