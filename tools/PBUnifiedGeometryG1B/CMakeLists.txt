cmake_minimum_required(VERSION 3.24)
project(PBUnifiedGeometryG1B LANGUAGES CXX)

# Independent diagnostic only; deliberately absent from the product build graph.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreadedDLL)
set(PB_G1B_REPO "" CACHE PATH "Explicit PixelBridge checkout")
set(PB_G1B_BASE_BUILD "" CACHE PATH "Read-only sealed Step2 Release build")
set(PB_G1B_FFMPEG "" CACHE PATH "Read-only sealed tool-only FFmpeg root")
set(PB_G1B_PYTHON "" CACHE FILEPATH "Explicit generator Python")
if(NOT PB_G1B_PYTHON)
    message(FATAL_ERROR "PB_G1B_PYTHON must point to an explicit Python interpreter")
endif()
if(NOT WIN32 OR NOT MSVC OR NOT EXISTS "${PB_G1B_BASE_BUILD}/libs/PBModulation/Release/PBModulation.lib")
    message(FATAL_ERROR "G1B requires Windows/MSVC and explicit existing Release libraries")
endif()
if(NOT EXISTS "${PB_G1B_FFMPEG}/include/libavcodec/avcodec.h")
    message(FATAL_ERROR "Explicit sealed FFmpeg required; no dependency installation")
endif()
execute_process(COMMAND "${PB_G1B_PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/prepare_reference.py"
    --repo "${PB_G1B_REPO}" --output "${CMAKE_CURRENT_BINARY_DIR}/generated" COMMAND_ERROR_IS_FATAL ANY)
set(dependencies "${PB_G1B_BASE_BUILD}/vcpkg_installed/x64-windows")
add_executable(PBUnifiedGeometryG1B main.cpp
    "${PB_G1B_REPO}/tools/PBUnifiedGeometryG1/fixtures.cpp"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/local_desktop_decode_g1b.cpp"
    "${PB_G1B_REPO}/tools/PBUnifiedRecordingReplay/recording_media.cpp")
target_compile_options(PBUnifiedGeometryG1B PRIVATE /utf-8 /W4 /WX /EHsc /permissive- /external:anglebrackets /external:W0)
target_compile_definitions(PBUnifiedGeometryG1B PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_include_directories(PBUnifiedGeometryG1B PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${PB_G1B_REPO}/tools/PBUnifiedGeometryG1"
    "${PB_G1B_REPO}/libs/PBModulation/src" "${PB_G1B_REPO}/tests/PBModulation"
    "${PB_G1B_REPO}/tools/PBUnifiedRecordingReplay" "${PB_G1B_REPO}/apps/common")
target_include_directories(PBUnifiedGeometryG1B SYSTEM PRIVATE "${dependencies}/include" "${PB_G1B_FFMPEG}/include")
foreach(library PBModulation PBCore PBProtocol PBInnerFec PBInterleave)
    target_include_directories(PBUnifiedGeometryG1B PRIVATE "${PB_G1B_REPO}/libs/${library}/include")
    target_link_libraries(PBUnifiedGeometryG1B PRIVATE "${PB_G1B_BASE_BUILD}/libs/${library}/Release/${library}.lib")
endforeach()
foreach(library blake3 libpng16 z)
    target_link_libraries(PBUnifiedGeometryG1B PRIVATE "${dependencies}/lib/${library}.lib")
endforeach()
target_link_libraries(PBUnifiedGeometryG1B PRIVATE bcrypt)
foreach(library avformat avcodec avutil swscale)
    target_link_libraries(PBUnifiedGeometryG1B PRIVATE "${PB_G1B_FFMPEG}/lib/${library}.lib")
endforeach()
foreach(dll blake3.dll libpng16.dll z.dll)
    add_custom_command(TARGET PBUnifiedGeometryG1B POST_BUILD COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${dependencies}/bin/${dll}" "$<TARGET_FILE_DIR:PBUnifiedGeometryG1B>" VERBATIM)
endforeach()
foreach(dll avformat-62.dll avcodec-62.dll avutil-60.dll swscale-9.dll)
    add_custom_command(TARGET PBUnifiedGeometryG1B POST_BUILD COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${PB_G1B_FFMPEG}/bin/${dll}" "$<TARGET_FILE_DIR:PBUnifiedGeometryG1B>" VERBATIM)
endforeach()
