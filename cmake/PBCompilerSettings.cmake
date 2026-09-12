# Central compiler and warning policy for all PixelBridge targets.
#
# Include convention (enforced by this policy):
#   - PixelBridge-owned headers: quoted includes ("pbcore/build_info.h")
#   - third-party headers:        angle-bracket includes (<catch2/...>)
# Targets that consume third-party headers add /external:anglebrackets and
# /external:W0 so that the strict policy still applies to owned code.

add_library(PBCompilerSettings INTERFACE)
add_library(PB::CompilerSettings ALIAS PBCompilerSettings)

target_compile_features(PBCompilerSettings INTERFACE cxx_std_20)

if(MSVC)
    target_compile_options(PBCompilerSettings INTERFACE
        /utf-8
        /W4
        /permissive-
        /EHsc
        /Zc:__cplusplus
        /Zc:preprocessor
        $<$<BOOL:${PB_TREAT_WARNINGS_AS_ERRORS}>:/WX>)
    # In-memory demodulation results carry the maximum accepted-Transport
    # buffers across all carriers; several live copies on one deep call chain
    # exceeded the MSVC 1 MiB default stack (LF4 replay probe). 4 MiB keeps
    # every thread of the process, including std::thread defaults, clear of
    # that ceiling; the cost is reserved address space only.
    target_link_options(PBCompilerSettings INTERFACE "/STACK:4194304")
endif()