cmake_minimum_required(VERSION 3.24)
project(PBRemoteThroughputStep3A LANGUAGES CXX)

# Standalone experiment. No product target, public input mode or dependency change.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreadedDLL)
set(PB_STEP3A_REPO "" CACHE PATH "Explicit PixelBridge checkout")
set(PB_STEP3A_BASE_BUILD "" CACHE PATH "Read-only sealed Step2 Release build")
if(NOT WIN32 OR NOT MSVC OR NOT EXISTS "${PB_STEP3A_BASE_BUILD}/apps/common/Release/PBApplication.lib")
    message(FATAL_ERROR "Windows/MSVC and the explicit sealed Step2 Release build are required")
endif()
set(dependencies "${PB_STEP3A_BASE_BUILD}/vcpkg_installed/x64-windows")
add_executable(PBRemoteThroughputStep3A main.cpp support.cpp scenario_source.cpp
    "${PB_STEP3A_REPO}/libs/PBRemoteVisualSimulator/src/channel_transform.cpp")
target_compile_options(PBRemoteThroughputStep3A PRIVATE /utf-8 /W4 /WX /EHsc /permissive- /external:anglebrackets /external:W0)
target_compile_definitions(PBRemoteThroughputStep3A PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_include_directories(PBRemoteThroughputStep3A PRIVATE "${PB_STEP3A_REPO}/apps/common" "${PB_STEP3A_REPO}/libs/PBRemoteVisualSimulator/include")
target_include_directories(PBRemoteThroughputStep3A SYSTEM PRIVATE "${dependencies}/include")
target_link_libraries(PBRemoteThroughputStep3A PRIVATE "${PB_STEP3A_BASE_BUILD}/apps/common/Release/PBApplication.lib")
foreach(library PBReceiver PBCompression PBOuterFec PBStorage PBRenderD3D PBScreenCaptureWgc PBScreenCaptureDxgi
    PBDemodD3D11 PBTelemetry PBDesktopLevelsReference PBModulation PBCore PBInnerFec PBInterleave
    PBRealCaptureReplay PBCaptureNormalize PBScreenRegion PBPresentTiming PBProtocol)
    target_include_directories(PBRemoteThroughputStep3A PRIVATE "${PB_STEP3A_REPO}/libs/${library}/include")
    target_link_libraries(PBRemoteThroughputStep3A PRIVATE "${PB_STEP3A_BASE_BUILD}/libs/${library}/Release/${library}.lib")
endforeach()
foreach(library zstd wirehair libpng16 z blake3)
    target_link_libraries(PBRemoteThroughputStep3A PRIVATE "${dependencies}/lib/${library}.lib")
endforeach()
target_link_libraries(PBRemoteThroughputStep3A PRIVATE windowsapp wtsapi32 gdi32 imm32 d3dcompiler runtimeobject user32 bcrypt Shcore d3d11 dxgi)
foreach(dll blake3.dll zstd.dll)
    add_custom_command(TARGET PBRemoteThroughputStep3A POST_BUILD COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${dependencies}/bin/${dll}" "$<TARGET_FILE_DIR:PBRemoteThroughputStep3A>" VERBATIM)
endforeach()
