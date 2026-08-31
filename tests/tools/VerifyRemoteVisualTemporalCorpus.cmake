if(NOT DEFINED PB_TEMPORAL_CORPUS_EXE OR NOT EXISTS "${PB_TEMPORAL_CORPUS_EXE}")
    message(FATAL_ERROR "PB_TEMPORAL_CORPUS_EXE is missing")
endif()

execute_process(
    COMMAND "${PB_TEMPORAL_CORPUS_EXE}"
    RESULT_VARIABLE pbTemporalResult
    OUTPUT_VARIABLE pbTemporalJson
    ERROR_VARIABLE pbTemporalError
    TIMEOUT 300)
if(NOT pbTemporalResult EQUAL 0)
    message(FATAL_ERROR "temporal corpus failed (${pbTemporalResult}): ${pbTemporalError}")
endif()

string(JSON pbTemporalSchema GET "${pbTemporalJson}" schema)
string(JSON pbTemporalEvents GET "${pbTemporalJson}" payload summary eventCount)
string(JSON pbTemporalDuplicates GET "${pbTemporalJson}" payload summary suppressedDuplicateEvents)
string(JSON pbTemporalReordered GET "${pbTemporalJson}" payload summary suppressedReorderedEvents)
string(JSON pbTemporalWrongIdentity GET "${pbTemporalJson}" payload summary wrongIdentityAcceptedTransportBlocks)
string(JSON pbTemporalCandidates GET "${pbTemporalJson}" payload summary wrongIdentityDiagnosticCandidates)
string(JSON pbTemporalSafe GET "${pbTemporalJson}" payload summary productionAdmissionSafe)
string(JSON pbTemporalExpectations GET "${pbTemporalJson}" payload summary expectationsMatched)
if(NOT pbTemporalSchema STREQUAL "PixelBridge.RemoteVisualTemporalCorpus.2" OR
    NOT pbTemporalEvents EQUAL 11 OR NOT pbTemporalDuplicates EQUAL 3 OR
    NOT pbTemporalReordered EQUAL 1 OR NOT pbTemporalWrongIdentity EQUAL 0 OR
    NOT pbTemporalCandidates EQUAL 4 OR NOT pbTemporalSafe OR NOT pbTemporalExpectations)
    message(FATAL_ERROR
        "unexpected temporal summary: schema=${pbTemporalSchema} events=${pbTemporalEvents} "
        "duplicates=${pbTemporalDuplicates} reordered=${pbTemporalReordered} "
        "wrongIdentity=${pbTemporalWrongIdentity} candidates=${pbTemporalCandidates} "
        "safe=${pbTemporalSafe} expectations=${pbTemporalExpectations}")
endif()
