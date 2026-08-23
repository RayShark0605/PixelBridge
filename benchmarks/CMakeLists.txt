add_executable(PBProtocolDescriptorStateBenchmark
    descriptor_state_benchmark.cpp)

add_executable(PBOuterFecWirehairV2Benchmark
    wirehair_v2_wrapper_benchmark.cpp)

add_executable(PBOuterFecDirectRepeatBenchmark
    direct_repeat_mode_benchmark.cpp)

target_link_libraries(PBProtocolDescriptorStateBenchmark
    PRIVATE
        PB::CompilerSettings
        PB::PBProtocol)

target_link_libraries(PBOuterFecWirehairV2Benchmark
    PRIVATE
        PB::CompilerSettings
        PB::PBOuterFec)

target_link_libraries(PBOuterFecDirectRepeatBenchmark
    PRIVATE
        PB::CompilerSettings
        PB::PBOuterFec)
