add_executable(PBProtocolDescriptorStateBenchmark
    descriptor_state_benchmark.cpp)

target_link_libraries(PBProtocolDescriptorStateBenchmark
    PRIVATE
        PB::CompilerSettings
        PB::PBProtocol)
