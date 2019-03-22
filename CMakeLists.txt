cmake_minimum_required(VERSION 3.5)

project(CrabNet)

set(CMAKE_CXX_STANDARD 11)

if( NOT APPLE )
    # check 64 bit
    if( CMAKE_SIZEOF_VOID_P MATCHES "4" )
      set( HAVE_64_BIT 0 )
    else(  CMAKE_SIZEOF_VOID_P MATCHES "4")
      set( HAVE_64_BIT 1 )
    endif( CMAKE_SIZEOF_VOID_P MATCHES "4")
endif( NOT APPLE )

IF (WIN32 AND NOT UNIX)
    set (PROGRAMFILESX86 $ENV{PROGRAMFILES})
    string(REPLACE "\\" "/" PROGRAMFILESX86 "${PROGRAMFILESX86}")
ENDIF(WIN32 AND NOT UNIX)

IF (WIN32 AND NOT UNIX)
    set(CRABNET_LIBRARY_LIBS ws2_32.lib)
    set(CRABNET_LIBRARY_SO_NAME RakNetDLL)
ELSE(WIN32 AND NOT UNIX)
    set(CRABNET_LIBRARY_LIBS pthread)
    set(CRABNET_LIBRARY_SO_NAME raknet)
ENDIF(WIN32 AND NOT UNIX)


# Options
option(CRABNET_ENABLE_SAMPLES "Generate RakNet sample projects." FALSE)
option(CRABNET_ENABLE_DLL "Generate the DLL project." TRUE)
option(CRABNET_ENABLE_STATIC "Generate the static library project." TRUE)

option(CRABNET_ENABLE_PVS "Enable PVS Studio Analyzer" FALSE)

option(CRABNET_ENABLE_LIBCAT_SECURITY "Enable secure connection support." FALSE)

set(CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/CmakeIncludes)

include(CmakeMacros)
include(PVS-Studio)

if(MSVC)
    FIXLINKOPTIONS()
    FIXCOMPILEOPTIONS()
endif(MSVC)


include_directories(SYSTEM include/raknet DependentExtensions)

add_subdirectory(lib)

set(CRABNET_COMMON_LIBS RakNetLibStatic)

if(CRABNET_ENABLE_SAMPLES)
    add_subdirectory(Samples)
endif()

file(COPY include DESTINATION .)
