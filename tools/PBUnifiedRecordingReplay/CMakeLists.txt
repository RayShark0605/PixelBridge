set(PB_STEP2_FFMPEG_ROOT "" CACHE PATH "Explicit tool-only pinned FFmpeg installation")
if(NOT EXISTS "${PB_STEP2_FFMPEG_ROOT}/include/libavcodec/avcodec.h")
    message(FATAL_ERROR "PB_STEP2_FFMPEG_ROOT must point to the separately pinned tool dependency")
endif()
add_executable(PBUnifiedRecordingReplay main.cpp recording_media.cpp fixture.cpp)
target_link_libraries(PBUnifiedRecordingReplay PRIVATE PB::CompilerSettings PB::PBApplication)
target_include_directories(PBUnifiedRecordingReplay SYSTEM PRIVATE "${PB_STEP2_FFMPEG_ROOT}/include")
foreach(library avformat avcodec avutil swscale)
    target_link_libraries(PBUnifiedRecordingReplay PRIVATE
        "${PB_STEP2_FFMPEG_ROOT}/$<IF:$<CONFIG:Debug>,debug/lib,lib>/${library}.lib")
endforeach()
target_compile_definitions(PBUnifiedRecordingReplay PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(PBUnifiedRecordingReplay PRIVATE /external:anglebrackets /external:W0)
# Pin runtime ABI DLL names and select the same configuration as the import libs.
foreach(dll avformat-62.dll avcodec-62.dll avutil-60.dll swscale-9.dll)
    add_custom_command(TARGET PBUnifiedRecordingReplay POST_BUILD COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${PB_STEP2_FFMPEG_ROOT}/$<IF:$<CONFIG:Debug>,debug/bin,bin>/${dll}" "$<TARGET_FILE_DIR:PBUnifiedRecordingReplay>" VERBATIM)
endforeach()
