PbAddUnitTest(PBScreenRegionTests test_geometry.cpp test_selection.cpp)
target_link_libraries(PBScreenRegionTests PRIVATE PB::PBScreenRegion)
target_include_directories(PBScreenRegionTests PRIVATE "${PROJECT_SOURCE_DIR}/libs/PBScreenRegion/src")
target_compile_definitions(PBScreenRegionTests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
PbInstrumentPresentationTest(PBScreenRegionTests)
set_tests_properties(PBScreenRegionTests PROPERTIES LABELS "screen-region;screen-region-unit" TIMEOUT 60)

add_executable(PBScreenRegionDpiProbe dpi_awareness_probe.cpp)
target_link_libraries(PBScreenRegionDpiProbe PRIVATE PB::CompilerSettings PB::PBScreenRegion)
target_compile_definitions(PBScreenRegionDpiProbe PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
if(MSVC)
    target_compile_options(PBScreenRegionDpiProbe PRIVATE /external:anglebrackets /external:W0)
endif()
PbInstrumentPresentationTest(PBScreenRegionDpiProbe)
set_property(GLOBAL APPEND PROPERTY PB_UNIT_TEST_TARGETS PBScreenRegionDpiProbe)
foreach(pbRegionAwareness IN ITEMS unaware system per-monitor-v1 thread-only-v2 process-v2-thread-system)
    add_test(NAME "PBScreenRegionRejectsHost.${pbRegionAwareness}" COMMAND PBScreenRegionDpiProbe "${pbRegionAwareness}")
    set_tests_properties("PBScreenRegionRejectsHost.${pbRegionAwareness}" PROPERTIES LABELS "screen-region;screen-region-unit" TIMEOUT 15)
endforeach()
unset(pbRegionAwareness)

# Compile the actual Decoder entry without the production manifest, so an
# environment/admission failure exercises its real exit-1/stderr branch. No
# failure switch or test-only command is added to the shipping application.
add_executable(PBScreenRegionDecoderHostProbe
    "${PROJECT_SOURCE_DIR}/apps/PixelBridgeDecoder/main.cpp"
    "${PROJECT_SOURCE_DIR}/apps/PixelBridgeDecoder/screen_region_cli.cpp")
target_link_libraries(PBScreenRegionDecoderHostProbe PRIVATE PB::CompilerSettings PB::PBCore PB::PBProtocol PB::PBScreenRegion)
target_compile_definitions(PBScreenRegionDecoderHostProbe PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
if(MSVC)
    target_compile_options(PBScreenRegionDecoderHostProbe PRIVATE /external:anglebrackets /external:W0)
endif()
PbInstrumentPresentationTest(PBScreenRegionDecoderHostProbe)
set_property(GLOBAL APPEND PROPERTY PB_UNIT_TEST_TARGETS PBScreenRegionDecoderHostProbe)
add_test(NAME PBScreenRegionDecoderRuntimeFailure
    COMMAND "${CMAKE_COMMAND}" "-DPB_HOST_PROBE=$<TARGET_FILE:PBScreenRegionDecoderHostProbe>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyDecoderHostFailure.cmake")
set_tests_properties(PBScreenRegionDecoderRuntimeFailure PROPERTIES LABELS "screen-region;screen-region-unit;decoder" TIMEOUT 15)
