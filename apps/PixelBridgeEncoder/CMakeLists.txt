# Application shell. The Qt-based UI is added in a later milestone; until
# then the entry point is a plain console program. Qt must never be linked
# from core libraries.
add_executable(PixelBridgeEncoder
    main.cpp)

target_link_libraries(PixelBridgeEncoder
    PRIVATE
        PB::CompilerSettings
        PB::PBCore
        PB::PBProtocol)