cmake_minimum_required(VERSION 3.24)
project(PBUnifiedGeometryG1 LANGUAGES CXX)

# Standalone only: never add this directory to the production tools graph.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreadedDLL)
set(PB_G1_REPO "" CACHE PATH "Explicit PixelBridge checkout")
set(PB_G1_BASE_BUILD "" CACHE PATH "Read-only sealed Step2 Release build")
set(PB_G1_PYTHON "<python>" CACHE FILEPATH "Explicit Python for isolated reference generation")
set(PB_G1_FFMPEG "" CACHE PATH "Explicit sealed tool-only FFmpeg root")
if(NOT WIN32 OR NOT MSVC OR NOT EXISTS "${PB_G1_BASE_BUILD}/libs/PBModulation/Release/PBModulation.lib")
    message(FATAL_ERROR "G1 requires Windows/MSVC and explicit existing Release libraries")
endif()
set(dependencies "${PB_G1_BASE_BUILD}/vcpkg_installed/x64-windows")
function(ConfigureG1Target targetName)
    target_compile_options(${targetName} PRIVATE /utf-8 /W4 /WX /EHsc /permissive- /external:anglebrackets /external:W0)
    target_compile_definitions(${targetName} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
    target_include_directories(${targetName} SYSTEM PRIVATE "${dependencies}/include")
    foreach(library PBModulation PBCore PBProtocol PBInnerFec PBInterleave)
        target_include_directories(${targetName} PRIVATE "${PB_G1_REPO}/libs/${library}/include")
        target_link_libraries(${targetName} PRIVATE "${PB_G1_BASE_BUILD}/libs/${library}/Release/${library}.lib")
    endforeach()
    foreach(library blake3 libpng16 z)
        target_link_libraries(${targetName} PRIVATE "${dependencies}/lib/${library}.lib")
    endforeach()
    target_link_libraries(${targetName} PRIVATE bcrypt)
    foreach(dll blake3.dll libpng16.dll z.dll)
        add_custom_command(TARGET ${targetName} POST_BUILD COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${dependencies}/bin/${dll}" "$<TARGET_FILE_DIR:${targetName}>" VERBATIM)
    endforeach()
endfunction()

execute_process(COMMAND "${PB_G1_PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/prepare_reference.py"
    --repo "${PB_G1_REPO}" --output "${CMAKE_CURRENT_BINARY_DIR}/generated" COMMAND_ERROR_IS_FATAL ANY)
if(NOT EXISTS "${PB_G1_FFMPEG}/include/libavcodec/avcodec.h")
    message(FATAL_ERROR "An explicit existing PB_G1_FFMPEG root is required; no dependency installation is performed")
endif()
add_executable(PBUnifiedGeometryG1 main.cpp fixtures.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/generated/unified_visual_g1.cpp"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/local_desktop_decode_g1.cpp"
    "${PB_G1_REPO}/tools/PBUnifiedRecordingReplay/recording_media.cpp")
target_include_directories(PBUnifiedGeometryG1 BEFORE PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated/include")
target_include_directories(PBUnifiedGeometryG1 PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${PB_G1_REPO}/libs/PBModulation/src"
    "${PB_G1_REPO}/tests/PBModulation" "${PB_G1_REPO}/tools/PBUnifiedRecordingReplay" "${PB_G1_REPO}/apps/common")
target_include_directories(PBUnifiedGeometryG1 SYSTEM PRIVATE "${PB_G1_FFMPEG}/include")
ConfigureG1Target(PBUnifiedGeometryG1)
foreach(library avformat avcodec avutil swscale)
    target_link_libraries(PBUnifiedGeometryG1 PRIVATE "${PB_G1_FFMPEG}/lib/${library}.lib")
endforeach()
foreach(dll avformat-62.dll avcodec-62.dll avutil-60.dll swscale-9.dll)
    add_custom_command(TARGET PBUnifiedGeometryG1 POST_BUILD COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${PB_G1_FFMPEG}/bin/${dll}" "$<TARGET_FILE_DIR:PBUnifiedGeometryG1>" VERBATIM)
endforeach()

# Original target and sources, built in this new tree; run only [fixed-canvas].
add_executable(PBLocalDesktopBootstrapTests
    "${PB_G1_REPO}/tests/PBModulation/test_bootstrap_rs.cpp"
    "${PB_G1_REPO}/tests/PBModulation/test_local_desktop_bootstrap.cpp"
    "${PB_G1_REPO}/tests/PBModulation/test_local_desktop_decode.cpp"
    "${PB_G1_REPO}/tests/PBModulation/test_local_desktop_luma_calibration.cpp"
    "${PB_G1_REPO}/tests/PBModulation/test_local_desktop_core_samples.cpp"
    "${PB_G1_REPO}/tests/PBModulation/test_local_desktop_partial_tear.cpp")
ConfigureG1Target(PBLocalDesktopBootstrapTests)
target_compile_definitions(PBLocalDesktopBootstrapTests PRIVATE PB_LOCAL_DESKTOP_GOLDEN_DIR="${PB_G1_REPO}/tests/golden/local-desktop-bootstrap")
target_link_libraries(PBLocalDesktopBootstrapTests PRIVATE "${dependencies}/lib/Catch2.lib" "${dependencies}/lib/manual-link/Catch2Main.lib")
if(EXISTS "${dependencies}/bin/Catch2.dll")
    add_custom_command(TARGET PBLocalDesktopBootstrapTests POST_BUILD COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${dependencies}/bin/Catch2.dll" "$<TARGET_FILE_DIR:PBLocalDesktopBootstrapTests>" VERBATIM)
endif()
