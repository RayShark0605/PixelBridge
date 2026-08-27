add_executable(PBScreenRegionGate screen_region_gate.cpp cli_gate.cpp native_supervisor.cpp
    "${PROJECT_SOURCE_DIR}/cmake/PixelBridge.PerMonitorV2.manifest")
target_link_libraries(PBScreenRegionGate PRIVATE PB::CompilerSettings PB::PBScreenRegion user32 gdi32 dxgi imm32)
target_include_directories(PBScreenRegionGate PRIVATE "${PROJECT_SOURCE_DIR}/libs/PBScreenRegion/src")
target_compile_definitions(PBScreenRegionGate PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
if(MSVC)
    target_compile_options(PBScreenRegionGate PRIVATE /external:anglebrackets /external:W0)
endif()
PbInstrumentPresentationTest(PBScreenRegionGate)
set_property(GLOBAL APPEND PROPERTY PB_UNIT_TEST_TARGETS PBScreenRegionGate)

set(pbRegionEvidence "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/evidence")
add_test(NAME PBScreenRegionGate.native COMMAND PBScreenRegionGate --native "${pbRegionEvidence}")
set_tests_properties(PBScreenRegionGate.native PROPERTIES
    LABELS "screen-region;screen-region-live" RUN_SERIAL TRUE RESOURCE_LOCK PixelBridgeDesktop TIMEOUT 180)
if(TARGET PixelBridgeDecoder)
    add_test(NAME PBScreenRegionGate.decoder COMMAND PBScreenRegionGate --decoder "$<TARGET_FILE:PixelBridgeDecoder>" "${pbRegionEvidence}")
    set_tests_properties(PBScreenRegionGate.decoder PROPERTIES
        LABELS "screen-region;screen-region-live;decoder" RUN_SERIAL TRUE RESOURCE_LOCK PixelBridgeDesktop TIMEOUT 90)
endif()
unset(pbRegionEvidence)
