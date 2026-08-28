add_library(PBDesktopLevelsReference STATIC)
add_library(PB::PBDesktopLevelsReference ALIAS PBDesktopLevelsReference)
target_sources(PBDesktopLevelsReference PRIVATE src/reference_channel.cpp
    PUBLIC FILE_SET HEADERS BASE_DIRS include FILES include/pbdesktoplevels/reference_channel.h)
target_include_directories(PBDesktopLevelsReference PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>)
target_compile_features(PBDesktopLevelsReference PUBLIC cxx_std_20)
target_link_libraries(PBDesktopLevelsReference PUBLIC PB::PBModulation
    PRIVATE PB::CompilerSettings PB::PBProtocol PB::PBInnerFec)
