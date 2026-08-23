# Configure-time architecture checks for PixelBridge core targets. These checks
# keep dependency violations from becoming silently accepted build behavior.

function(PbResolveAliasedTarget targetName outputVariable)
    get_target_property(aliasedTarget "${targetName}" ALIASED_TARGET)
    if(aliasedTarget)
        set(${outputVariable} "${aliasedTarget}" PARENT_SCOPE)
    else()
        set(${outputVariable} "${targetName}" PARENT_SCOPE)
    endif()
endfunction()

function(PbAssertNoQtDependency rootTarget currentTarget visitedTargets)
    PbResolveAliasedTarget("${currentTarget}" resolvedTarget)

    foreach(targetIdentity IN ITEMS "${currentTarget}" "${resolvedTarget}")
        if(targetIdentity MATCHES "^Qt([0-9]+)?::")
            message(FATAL_ERROR
                "Core target ${rootTarget} has a forbidden Qt dependency "
                "through ${currentTarget} (resolved target: ${resolvedTarget})")
        endif()
    endforeach()

    list(FIND visitedTargets "${resolvedTarget}" visitedIndex)
    if(NOT visitedIndex EQUAL -1)
        return()
    endif()
    list(APPEND visitedTargets "${resolvedTarget}")

    foreach(linkProperty IN ITEMS LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
        get_target_property(linkItems "${resolvedTarget}" ${linkProperty})
        if(NOT linkItems)
            continue()
        endif()

        string(JOIN ";" joinedLinkItems ${linkItems})
        if(joinedLinkItems MATCHES "(^|[^A-Za-z0-9_])Qt([0-9]+)?::")
            message(FATAL_ERROR
                "Core target ${rootTarget} has a forbidden Qt dependency "
                "through ${resolvedTarget}: ${joinedLinkItems}")
        endif()

        foreach(linkItem IN LISTS linkItems)
            string(REGEX MATCHALL
                "[A-Za-z0-9_][A-Za-z0-9_.+-]*(::[A-Za-z0-9_][A-Za-z0-9_.+-]*)*"
                candidateTargets
                "${linkItem}")
            foreach(candidateTarget IN LISTS candidateTargets)
                if(TARGET "${candidateTarget}")
                    PbAssertNoQtDependency(
                        "${rootTarget}"
                        "${candidateTarget}"
                        "${visitedTargets}")
                endif()
            endforeach()
        endforeach()
    endforeach()
endfunction()

function(PbValidateTargetBoundary targetName)
    if(NOT TARGET "${targetName}")
        message(FATAL_ERROR "Cannot validate missing target: ${targetName}")
    endif()

    PbAssertNoQtDependency("${targetName}" "${targetName}" "")

    get_target_property(interfaceIncludeDirectories
        "${targetName}"
        INTERFACE_INCLUDE_DIRECTORIES)
    foreach(includeDirectory IN LISTS interfaceIncludeDirectories)
        string(REPLACE "\\" "/" normalizedIncludeDirectory "${includeDirectory}")
        if(normalizedIncludeDirectory MATCHES "/src([/>;]|$)")
            message(FATAL_ERROR
                "Core target ${targetName} exposes a private src directory: "
                "${includeDirectory}")
        endif()
    endforeach()

    get_target_property(interfaceCompileOptions
        "${targetName}"
        INTERFACE_COMPILE_OPTIONS)
    if(interfaceCompileOptions)
        string(JOIN ";" joinedInterfaceCompileOptions ${interfaceCompileOptions})
        if(joinedInterfaceCompileOptions MATCHES
                "(/W[0-4]|/Wall|/WX|/permissive-|/EH[^;>]*|/Zc:preprocessor)")
            message(FATAL_ERROR
                "Core target ${targetName} exposes internal compiler policy: "
                "${joinedInterfaceCompileOptions}")
        endif()
    endif()

    get_target_property(interfaceLinkLibraries
        "${targetName}"
        INTERFACE_LINK_LIBRARIES)
    foreach(interfaceLinkLibrary IN LISTS interfaceLinkLibraries)
        if(interfaceLinkLibrary STREQUAL "PB::CompilerSettings"
                OR interfaceLinkLibrary STREQUAL "PBCompilerSettings")
            message(FATAL_ERROR
                "Core target ${targetName} publicly links the internal "
                "compiler policy target: ${interfaceLinkLibrary}")
        endif()
    endforeach()
endfunction()

function(PbCollectDirectoryTargets directoryPath outputVariable)
    get_property(directoryTargets
        DIRECTORY "${directoryPath}"
        PROPERTY BUILDSYSTEM_TARGETS)
    get_property(subdirectories
        DIRECTORY "${directoryPath}"
        PROPERTY SUBDIRECTORIES)

    set(collectedTargets ${directoryTargets})
    foreach(subdirectory IN LISTS subdirectories)
        PbCollectDirectoryTargets("${subdirectory}" subdirectoryTargets)
        list(APPEND collectedTargets ${subdirectoryTargets})
    endforeach()

    list(REMOVE_DUPLICATES collectedTargets)
    set(${outputVariable} "${collectedTargets}" PARENT_SCOPE)
endfunction()

function(PbValidateCorePublicHeaders librariesDirectory)
    file(GLOB_RECURSE corePublicHeaders CONFIGURE_DEPENDS
        "${librariesDirectory}/*/include/*.h"
        "${librariesDirectory}/*/include/*.hh"
        "${librariesDirectory}/*/include/*.hpp"
        "${librariesDirectory}/*/include/*.hxx")

    foreach(publicHeader IN LISTS corePublicHeaders)
        file(STRINGS "${publicHeader}" qtIncludeLines
            REGEX "^[ \t]*#[ \t]*include[ \t]*[<\"](Q[A-Z][A-Za-z0-9_]*|Qt[A-Za-z0-9_/.-]*|q[a-z][A-Za-z0-9_]*\\.h|qt[a-z0-9_/.-]*)[>\"]")
        if(qtIncludeLines)
            message(FATAL_ERROR
                "Core public header ${publicHeader} includes Qt: ${qtIncludeLines}")
        endif()

        file(STRINGS "${publicHeader}" wirehairIncludeLines
            REGEX "^[ \t]*#[ \t]*include[ \t]*[<\"]wirehair/")
        if(wirehairIncludeLines)
            message(FATAL_ERROR
                "Core public header ${publicHeader} exposes Wirehair: "
                "${wirehairIncludeLines}")
        endif()
    endforeach()
endfunction()

function(PbValidateCoreTargetBoundaries librariesDirectory)
    PbCollectDirectoryTargets("${librariesDirectory}" coreTargets)
    if(NOT coreTargets)
        message(FATAL_ERROR
            "No core targets were registered below ${librariesDirectory}")
    endif()

    foreach(coreTarget IN LISTS coreTargets)
        PbValidateTargetBoundary("${coreTarget}")
    endforeach()

    foreach(staticBaselineTarget IN ITEMS
            PBCore PBProtocol PBCompression PBOuterFec)
        if(TARGET "${staticBaselineTarget}")
            get_target_property(targetType "${staticBaselineTarget}" TYPE)
            if(NOT targetType STREQUAL "STATIC_LIBRARY")
                message(FATAL_ERROR
                    "${staticBaselineTarget} must remain a STATIC_LIBRARY, got ${targetType}")
            endif()
        endif()
    endforeach()

    if(TARGET PBProtocol)
        get_target_property(protocolLinkLibraries PBProtocol LINK_LIBRARIES)
        get_target_property(protocolInterfaceLinkLibraries
            PBProtocol
            INTERFACE_LINK_LIBRARIES)
        string(JOIN ";" protocolDependencyDeclarations
            ${protocolLinkLibraries}
            ${protocolInterfaceLinkLibraries})
        if(protocolDependencyDeclarations MATCHES "PB::PBCore|(^|;)PBCore($|;)")
            message(FATAL_ERROR
                "PBProtocol must not depend on PBCore: "
                "${protocolDependencyDeclarations}")
        endif()
    endif()

    PbValidateCorePublicHeaders("${librariesDirectory}")
endfunction()
