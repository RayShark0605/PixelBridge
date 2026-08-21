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
endif()