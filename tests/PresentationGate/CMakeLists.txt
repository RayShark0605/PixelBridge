add_executable(PBPresentationGate
    presentation_gate.cpp
    mode_supervisor.cpp
    "${PROJECT_SOURCE_DIR}/cmake/PixelBridge.PerMonitorV2.manifest")
target_link_libraries(PBPresentationGate PRIVATE
    PB::CompilerSettings PB::PBApplication PB::PBRenderD3D PB::PBModulation PB::PBProtocol user32)
target_include_directories(PBPresentationGate PRIVATE "${PROJECT_SOURCE_DIR}/libs/PBRenderD3D/src")
PbInstrumentPresentationTest(PBPresentationGate)
target_compile_definitions(PBPresentationGate PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
    "PB_REMOTE_VISUAL_LF4_GOLDEN_DIR=\"${PROJECT_SOURCE_DIR}/tests/golden/remote-visual/lf4\"")
if(MSVC)
    target_compile_options(PBPresentationGate PRIVATE /external:anglebrackets /external:W0)
endif()

set(pbPresentationEvidence "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/evidence")
foreach(pbPresentationMode IN ITEMS gpu-warp gpu-hardware lf4-encoder-warp lf4-encoder-hardware lf4-production-encoder live mode-supervisor)
    add_test(NAME "PBPresentationGate.${pbPresentationMode}"
        COMMAND PBPresentationGate "--${pbPresentationMode}" "${pbPresentationEvidence}")
    set_tests_properties("PBPresentationGate.${pbPresentationMode}" PROPERTIES
        LABELS "presentation;presentation-live"
        RUN_SERIAL TRUE
        RESOURCE_LOCK PixelBridgeDesktop
        TIMEOUT 240)
endforeach()
if(TARGET PixelBridgeEncoder)
    add_test(NAME PBPresentationGate.encoder
        COMMAND "${CMAKE_COMMAND}"
            "-DPB_ENCODER=$<TARGET_FILE:PixelBridgeEncoder>"
            "-DPB_EVIDENCE_ROOT=${pbPresentationEvidence}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyEncoderEntry.cmake")
    set_tests_properties(PBPresentationGate.encoder PROPERTIES
        LABELS "presentation;presentation-live;encoder"
        RUN_SERIAL TRUE RESOURCE_LOCK PixelBridgeDesktop TIMEOUT 60)
endif()
set_property(GLOBAL APPEND PROPERTY PB_UNIT_TEST_TARGETS PBPresentationGate)
unset(pbPresentationEvidence)
unset(pbPresentationMode)
