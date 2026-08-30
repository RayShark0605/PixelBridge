# Tool targets are registered here as they are implemented. Keeping this
# subtree optional prevents command-line tooling dependencies from entering
# the core library or application configuration paths.

add_executable(PBVectorGen PBVectorGen/main.cpp)
target_link_libraries(PBVectorGen
    PRIVATE
        PB::CompilerSettings
        PB::PBGoldenVector
        PB::PBInterleave
        PB::PBModulation
        PB::PBProtocol)

# PBProtocolDump: deterministic record inspector (core + CLI). The core is
# also linked by the tests/tools Catch2 suite.
add_library(PBProtocolDumpCore STATIC PBProtocolDump/protocol_dump_core.cpp)
target_link_libraries(PBProtocolDumpCore
    PUBLIC
        PB::PBProtocol
        PB::PBModulation
    PRIVATE
        PB::CompilerSettings)
target_include_directories(PBProtocolDumpCore
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/PBProtocolDump)

add_executable(PBProtocolDump PBProtocolDump/main.cpp)
target_link_libraries(PBProtocolDump
    PRIVATE
        PB::CompilerSettings
        PBProtocolDumpCore)

# PBFrameInspector: deterministic reference-frame inspector (core + CLI).
add_library(PBFrameInspectorCore STATIC
    PBFrameInspector/frame_inspector_core.cpp)
target_link_libraries(PBFrameInspectorCore
    PUBLIC
        PB::PBProtocol
        PB::PBModulation
        PB::PBInnerFec
    PRIVATE
        PB::CompilerSettings)
target_include_directories(PBFrameInspectorCore
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/PBFrameInspector)

add_executable(PBFrameInspector PBFrameInspector/main.cpp)
target_link_libraries(PBFrameInspector
    PRIVATE
        PB::CompilerSettings
        PBFrameInspectorCore)

add_executable(PBDesktopLevelsBaseline PBDesktopLevelsBaseline/main.cpp)
target_link_libraries(PBDesktopLevelsBaseline PRIVATE PB::CompilerSettings PB::PBDesktopLevelsReference PB::PBProtocol)

# PBRemoteVisualChannelMatrix: deterministic, headless LF4 channel corpus.
# The core is linked by tests so the exact JSON and truth-boundary
# classifications can be verified without launching a process or a window.
add_library(PBRemoteVisualChannelMatrixCore STATIC
    PBRemoteVisualChannelMatrix/channel_matrix_core.cpp)
target_link_libraries(PBRemoteVisualChannelMatrixCore
    PUBLIC
        PB::PBDesktopLevelsReference
        PB::PBModulation
        PB::PBProtocol
        PB::PBRemoteVisualSimulator
    PRIVATE
        PB::CompilerSettings)
target_include_directories(PBRemoteVisualChannelMatrixCore
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/PBRemoteVisualChannelMatrix)

add_executable(PBRemoteVisualChannelMatrix
    PBRemoteVisualChannelMatrix/main.cpp)
target_link_libraries(PBRemoteVisualChannelMatrix
    PRIVATE
        PB::CompilerSettings
        PBRemoteVisualChannelMatrixCore)
