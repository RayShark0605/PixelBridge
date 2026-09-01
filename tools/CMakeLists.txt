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

# Shared headless evidence bridge. It feeds production-accepted LF4 Transport
# blocks through the existing bounded Receiver/Outer/segment/digest contracts;
# it owns no alternate protocol, FEC, or file-recovery implementation.
add_library(PBRemoteVisualReceiverEvidenceCore STATIC
    PBRemoteVisualReceiverEvidence/receiver_evidence.cpp)
target_link_libraries(PBRemoteVisualReceiverEvidenceCore
    PUBLIC
        PB::PBDesktopLevelsReference
        PB::PBProtocol
        PB::PBReceiver
    PRIVATE
        PB::CompilerSettings)
target_include_directories(PBRemoteVisualReceiverEvidenceCore
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/PBRemoteVisualReceiverEvidence)

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
        PBRemoteVisualReceiverEvidenceCore
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

# PBRemoteVisualCodecProbe exports a deterministic production LF4 sequence and
# evaluates decoded Gray8 frames through the existing FEC/Transport boundary.
# External codec orchestration remains in a separate evidence script.
add_library(PBRemoteVisualCodecProbeCore STATIC
    PBRemoteVisualCodecProbe/codec_probe_core.cpp)
target_link_libraries(PBRemoteVisualCodecProbeCore
    PUBLIC
        PB::PBDesktopLevelsReference
        PB::PBModulation
        PB::PBProtocol
        PBRemoteVisualReceiverEvidenceCore
    PRIVATE
        PB::CompilerSettings)
target_include_directories(PBRemoteVisualCodecProbeCore
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/PBRemoteVisualCodecProbe)

add_executable(PBRemoteVisualCodecProbe
    PBRemoteVisualCodecProbe/main.cpp)
target_link_libraries(PBRemoteVisualCodecProbe
    PRIVATE
        PB::CompilerSettings
        PBRemoteVisualCodecProbeCore)

# PBRemoteVisualReplayInspector validates a sealed receiver-only Replay v2 and
# independently decodes each capture through the existing CPU/reference
# modulation plus production Transport trust boundary. It never uses sender
# truth and therefore reports false-accept counts as unavailable.
add_library(PBRemoteVisualReplayInspectorCore STATIC
    PBRemoteVisualReplayInspector/replay_inspector_core.cpp)
target_link_libraries(PBRemoteVisualReplayInspectorCore
    PUBLIC
        PB::PBDesktopLevelsReference
        PB::PBRealCaptureReplay
    PRIVATE
        PB::CompilerSettings
        PB::PBModulation
        PB::PBProtocol)
target_include_directories(PBRemoteVisualReplayInspectorCore
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/PBRemoteVisualReplayInspector)

add_executable(PBRemoteVisualReplayInspector
    PBRemoteVisualReplayInspector/main.cpp)
target_link_libraries(PBRemoteVisualReplayInspector
    PRIVATE
        PB::CompilerSettings
        PBRemoteVisualReplayInspectorCore)

# Evidence-only sender used to obtain real receiver-side Replay v2 captures.
# LF4 remains absent from the product profile enum/GUI: this tool presents one
# deterministic diagnostic raster and performs no file-transfer lifecycle.
add_executable(PBRemoteVisualEvidencePresenter
    PBRemoteVisualEvidencePresenter/main.cpp
    "${PROJECT_SOURCE_DIR}/cmake/PixelBridge.PerMonitorV2.manifest")
target_link_libraries(PBRemoteVisualEvidencePresenter
    PRIVATE
        PB::CompilerSettings
        PB::PBDesktopLevelsReference
        PB::PBModulation
        PB::PBProtocol
        PB::PBRenderD3D)

# The temporal corpus intentionally reuses the production application identity,
# duplicate-refinement and stall trackers. It is available only when the app
# model is built; the tool itself is headless and starts no capture or window.
if(TARGET PBApplication)
    add_library(PBRemoteVisualTemporalCorpusCore STATIC
        PBRemoteVisualTemporalCorpus/temporal_corpus_core.cpp)
    target_link_libraries(PBRemoteVisualTemporalCorpusCore
        PUBLIC
            PB::PBApplication
            PBRemoteVisualCodecProbeCore
        PRIVATE
            PB::CompilerSettings)
    target_include_directories(PBRemoteVisualTemporalCorpusCore
        PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/PBRemoteVisualTemporalCorpus)

    add_executable(PBRemoteVisualTemporalCorpus
        PBRemoteVisualTemporalCorpus/main.cpp)
    target_link_libraries(PBRemoteVisualTemporalCorpus
        PRIVATE
            PB::CompilerSettings
            PBRemoteVisualTemporalCorpusCore)
endif()
