# Raspberry Pi 64-bit Linux cross-compilation toolchain for YuanBook.
#
# Required compiler prefix defaults to aarch64-none-linux-gnu (matching the Arm GNU Toolchain used by cpHomeCenter). Override with either:
#   -DRPI_TOOLCHAIN_PREFIX=<prefix>
#   $env:RPI_TOOLCHAIN_PREFIX=<prefix>
#
# Optional locations:
#   -DRPI_TOOLCHAIN_ROOT=<dir> or $env:RPI_TOOLCHAIN_ROOT=<dir>
#   -DRPI_SYSROOT=<dir>        or $env:RPI_SYSROOT=<dir>

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

if(NOT DEFINED RPI_TOOLCHAIN_PREFIX OR RPI_TOOLCHAIN_PREFIX STREQUAL "")
    if(DEFINED ENV{RPI_TOOLCHAIN_PREFIX} AND NOT "$ENV{RPI_TOOLCHAIN_PREFIX}" STREQUAL "")
        set(RPI_TOOLCHAIN_PREFIX "$ENV{RPI_TOOLCHAIN_PREFIX}" CACHE STRING "GNU cross compiler target prefix" FORCE)
    else()
        set(RPI_TOOLCHAIN_PREFIX "aarch64-none-linux-gnu" CACHE STRING "GNU cross compiler target prefix" FORCE)
    endif()
endif()

if(NOT DEFINED RPI_TOOLCHAIN_ROOT OR RPI_TOOLCHAIN_ROOT STREQUAL "")
    if(DEFINED ENV{RPI_TOOLCHAIN_ROOT} AND NOT "$ENV{RPI_TOOLCHAIN_ROOT}" STREQUAL "")
        set(RPI_TOOLCHAIN_ROOT "$ENV{RPI_TOOLCHAIN_ROOT}" CACHE PATH "Directory containing cross compiler executables" FORCE)
    endif()
endif()

if(NOT DEFINED RPI_SYSROOT OR RPI_SYSROOT STREQUAL "")
    if(DEFINED ENV{RPI_SYSROOT} AND NOT "$ENV{RPI_SYSROOT}" STREQUAL "")
        set(RPI_SYSROOT "$ENV{RPI_SYSROOT}" CACHE PATH "Target Raspberry Pi sysroot" FORCE)
    endif()
endif()

set(_RPI_TOOLCHAIN_HINTS)
if(DEFINED RPI_TOOLCHAIN_ROOT AND NOT RPI_TOOLCHAIN_ROOT STREQUAL "")
    list(APPEND _RPI_TOOLCHAIN_HINTS
        "${RPI_TOOLCHAIN_ROOT}"
        "${RPI_TOOLCHAIN_ROOT}/bin"
    )
endif()

find_program(_RPI_C_COMPILER
    NAMES ${RPI_TOOLCHAIN_PREFIX}-gcc
    HINTS ${_RPI_TOOLCHAIN_HINTS}
    NO_CACHE
)
find_program(_RPI_CXX_COMPILER
    NAMES ${RPI_TOOLCHAIN_PREFIX}-g++
    HINTS ${_RPI_TOOLCHAIN_HINTS}
    NO_CACHE
)

if(NOT _RPI_C_COMPILER OR NOT _RPI_CXX_COMPILER)
    message(FATAL_ERROR
        "Raspberry Pi aarch64 cross compiler was not found. "
        "Install/provide '${RPI_TOOLCHAIN_PREFIX}-gcc' and '${RPI_TOOLCHAIN_PREFIX}-g++', "
        "or pass -DRPI_TOOLCHAIN_ROOT=<toolchain-bin-or-root> / set RPI_TOOLCHAIN_ROOT."
    )
endif()

set(CMAKE_C_COMPILER "${_RPI_C_COMPILER}" CACHE FILEPATH "Raspberry Pi C compiler" FORCE)
set(CMAKE_CXX_COMPILER "${_RPI_CXX_COMPILER}" CACHE FILEPATH "Raspberry Pi C++ compiler" FORCE)

if(DEFINED RPI_SYSROOT AND NOT RPI_SYSROOT STREQUAL "")
    set(CMAKE_SYSROOT "${RPI_SYSROOT}" CACHE PATH "Target Raspberry Pi sysroot" FORCE)
    set(CMAKE_FIND_ROOT_PATH "${RPI_SYSROOT}" CACHE STRING "CMake target find root" FORCE)
endif()

# Keep host tools discoverable, but resolve target headers/libraries/packages from the sysroot when provided.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Avoid try_run during cross configuration.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
