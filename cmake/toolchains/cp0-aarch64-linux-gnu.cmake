#
# SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
#
# SPDX-License-Identifier: MIT
#
# CardputerZero aarch64 cross toolchain.
# The BSP under .cache/sdk_bsp-src is a sysroot: it contains target headers,
# target libraries, startup objects, and pkg-config metadata, but no CMake project.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CM0_MULTIARCH aarch64-linux-gnu CACHE STRING "Target Debian multiarch tuple")

get_filename_component(_CP0_PROJECT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(_CP0_CACHE_DIR "${_CP0_PROJECT_ROOT}/.cache")
set(_CP0_DEFAULT_SYSROOT "${_CP0_CACHE_DIR}/sdk_bsp-src")

set(CM0_SDK_ROOT "${_CP0_DEFAULT_SYSROOT}" CACHE PATH "Path to CM0/CardputerZero BSP sysroot")
set(CM0_SDK_VERSION "v0.0.6" CACHE STRING "CM0/CardputerZero BSP release version")
set(CM0_SDK_URL "https://github.com/CardputerZero/M5CardputerZero-UserDemo/releases/download/${CM0_SDK_VERSION}/sdk_bsp.tar.gz" CACHE STRING "CM0/CardputerZero BSP archive URL")
set(CM0_ALLOW_FETCH_DEPS ON CACHE BOOL "Allow downloading the CM0/CardputerZero BSP when the sysroot is missing")

# CMAKE_SYSROOT must exist before project() runs, otherwise CMake's compiler ABI
# checks fail before the main CMakeLists.txt has a chance to download anything.
if(NOT EXISTS "${CM0_SDK_ROOT}")
    if(NOT CM0_ALLOW_FETCH_DEPS)
        message(FATAL_ERROR
            "CM0 SDK sysroot not found: ${CM0_SDK_ROOT}\n"
            "Enable -DCM0_ALLOW_FETCH_DEPS=ON or provide -DCM0_SDK_ROOT=<path>."
        )
    endif()

    file(MAKE_DIRECTORY "${_CP0_CACHE_DIR}")
    get_filename_component(_CP0_SYSROOT_PARENT "${CM0_SDK_ROOT}" DIRECTORY)
    file(MAKE_DIRECTORY "${_CP0_SYSROOT_PARENT}")
    set(_CP0_SDK_ARCHIVE "${_CP0_CACHE_DIR}/sdk_bsp.tar.gz")

    # Always fetch to a fresh temp file: the release asset redirects to a signed
    # CDN URL that ignores Range, so resuming an existing partial appends a second
    # copy instead of continuing it. Rename only once the transfer succeeded.
    message(STATUS "CM0 SDK sysroot missing, downloading BSP: ${CM0_SDK_URL}")
    file(REMOVE "${_CP0_SDK_ARCHIVE}.part")
    file(DOWNLOAD
        "${CM0_SDK_URL}"
        "${_CP0_SDK_ARCHIVE}.part"
        SHOW_PROGRESS
        STATUS _CP0_DOWNLOAD_STATUS
        INACTIVITY_TIMEOUT 60
    )
    list(GET _CP0_DOWNLOAD_STATUS 0 _CP0_DOWNLOAD_CODE)
    list(GET _CP0_DOWNLOAD_STATUS 1 _CP0_DOWNLOAD_MESSAGE)
    if(NOT _CP0_DOWNLOAD_CODE EQUAL 0)
        file(REMOVE "${_CP0_SDK_ARCHIVE}.part")
        message(FATAL_ERROR "Failed to download CM0 BSP: ${_CP0_DOWNLOAD_MESSAGE}")
    endif()
    file(RENAME "${_CP0_SDK_ARCHIVE}.part" "${_CP0_SDK_ARCHIVE}")

    # Extract to a staging dir for the same reason: CM0_SDK_ROOT existing is what
    # marks the BSP as installed, so a failed extract must not create it.
    message(STATUS "Extracting CM0 BSP to ${CM0_SDK_ROOT}")
    file(REMOVE_RECURSE "${CM0_SDK_ROOT}.part")
    file(MAKE_DIRECTORY "${CM0_SDK_ROOT}.part")
    file(ARCHIVE_EXTRACT INPUT "${_CP0_SDK_ARCHIVE}" DESTINATION "${CM0_SDK_ROOT}.part")
    file(RENAME "${CM0_SDK_ROOT}.part" "${CM0_SDK_ROOT}")
endif()

if(NOT EXISTS "${CM0_SDK_ROOT}/usr/include" OR NOT EXISTS "${CM0_SDK_ROOT}/usr/lib")
    file(GLOB _CP0_EXTRACTED_DIRS LIST_DIRECTORIES true "${CM0_SDK_ROOT}/*")
    foreach(_CP0_EXTRACTED_DIR IN LISTS _CP0_EXTRACTED_DIRS)
        if(EXISTS "${_CP0_EXTRACTED_DIR}/usr/include" AND EXISTS "${_CP0_EXTRACTED_DIR}/usr/lib")
            set(CM0_SDK_ROOT "${_CP0_EXTRACTED_DIR}" CACHE PATH "Path to CM0/CardputerZero BSP sysroot" FORCE)
            message(STATUS "Using extracted CM0 sysroot: ${CM0_SDK_ROOT}")
            break()
        endif()
    endforeach()
endif()

if(NOT EXISTS "${CM0_SDK_ROOT}/usr/include" OR NOT EXISTS "${CM0_SDK_ROOT}/usr/lib")
    message(FATAL_ERROR
        "CM0 SDK sysroot appears incomplete: ${CM0_SDK_ROOT}\n"
        "Expected usr/include and usr/lib after extracting sdk_bsp.tar.gz."
    )
endif()

set(CMAKE_SYSROOT "${CM0_SDK_ROOT}" CACHE PATH "Sysroot used for CardputerZero cross builds" FORCE)
set(CMAKE_SYSROOT_COMPILE "${CMAKE_SYSROOT}" CACHE PATH "Compile sysroot used for CardputerZero cross builds" FORCE)
set(CMAKE_SYSROOT_LINK "${CMAKE_SYSROOT}" CACHE PATH "Link sysroot used for CardputerZero cross builds" FORCE)

find_program(_CP0_C_COMPILER NAMES aarch64-linux-gnu-gcc REQUIRED)
find_program(_CP0_CXX_COMPILER NAMES aarch64-linux-gnu-g++ REQUIRED)
set(CMAKE_C_COMPILER "${_CP0_C_COMPILER}" CACHE FILEPATH "aarch64 Linux C compiler" FORCE)
set(CMAKE_CXX_COMPILER "${_CP0_CXX_COMPILER}" CACHE FILEPATH "aarch64 Linux C++ compiler" FORCE)
set(CMAKE_LIBRARY_ARCHITECTURE "${CM0_MULTIARCH}")

# Cross builds never run from the build tree, so link with the install RPATH
# directly. This also avoids CMake's install-time relink, which Ninja refuses to
# do because CMAKE_EXECUTABLE_FORMAT is not detected under this sysroot.
set(CMAKE_BUILD_WITH_INSTALL_RPATH ON)

set(CMAKE_FIND_ROOT_PATH "${CMAKE_SYSROOT}" CACHE STRING "Root paths for cross find_* calls" FORCE)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

list(PREPEND CMAKE_PREFIX_PATH
    "${CMAKE_SYSROOT}/usr"
    "${CMAKE_SYSROOT}/usr/lib/${CM0_MULTIARCH}/cmake"
    "${CMAKE_SYSROOT}/usr/local"
)

# Make pkg-config resolve BSP metadata as target paths instead of host paths.
set(ENV{PKG_CONFIG_SYSROOT_DIR} "${CMAKE_SYSROOT}")
set(ENV{PKG_CONFIG_LIBDIR}
    "${CMAKE_SYSROOT}/usr/lib/${CM0_MULTIARCH}/pkgconfig:${CMAKE_SYSROOT}/usr/lib/pkgconfig:${CMAKE_SYSROOT}/usr/share/pkgconfig"
)

# The BSP includes GCC runtime/startup files. Use them during link tests and
# final links so crt*.o/libstdc++/libgcc are resolved from the sysroot first.
file(GLOB _CP0_GCC_RUNTIME_DIRS LIST_DIRECTORIES true "${CMAKE_SYSROOT}/usr/lib/gcc/${CM0_MULTIARCH}/*")
set(_CP0_MULTIARCH_LIB_DIR "${CMAKE_SYSROOT}/usr/lib/${CM0_MULTIARCH}")
if(_CP0_GCC_RUNTIME_DIRS)
    list(SORT _CP0_GCC_RUNTIME_DIRS COMPARE NATURAL ORDER DESCENDING)
    list(GET _CP0_GCC_RUNTIME_DIRS 0 CP0_GCC_RUNTIME_DIR)
    set(CP0_GCC_RUNTIME_DIR "${CP0_GCC_RUNTIME_DIR}" CACHE PATH "GCC runtime directory inside CM0 sysroot")
    set(_CP0_SYSROOT_LINK_FLAGS
        "-B${_CP0_MULTIARCH_LIB_DIR}/ -B${CP0_GCC_RUNTIME_DIR}/ -Wl,-rpath-link,${_CP0_MULTIARCH_LIB_DIR} -L${_CP0_MULTIARCH_LIB_DIR}"
    )
    set(CMAKE_EXE_LINKER_FLAGS_INIT "${CMAKE_EXE_LINKER_FLAGS_INIT} ${_CP0_SYSROOT_LINK_FLAGS}")
    set(CMAKE_SHARED_LINKER_FLAGS_INIT "${CMAKE_SHARED_LINKER_FLAGS_INIT} ${_CP0_SYSROOT_LINK_FLAGS}")
endif()

# Libc headers must come from the BSP sysroot, matching the glibc we link and
# run against. Ubuntu/Debian gcc-cross searches the host package
# libc6-dev-arm64-cross (/usr/aarch64-linux-gnu/include) BEFORE the sysroot,
# so a newer host glibc (2.43) silently paired its headers with the sysroot's
# 2.41 libc. glibc 2.42 changed the termios B* values, so every baud change
# failed at runtime. Rebuild the compiler's include list with -nostdinc, keep
# its own dirs (libstdc++, gcc builtins), point libc at the sysroot, and drop
# the host libc dir. src/platform/sysroot_abi_guard.cpp static_asserts the
# result via CP0_SYSROOT_GLIBC_MINOR.
option(CP0_ISOLATE_SYSROOT_HEADERS "Use only the BSP sysroot libc headers (drop the host cross libc include dir)" ON)

# try_compile() projects re-read this file in a fresh cache: forward every
# user-settable variable it reads so nested probes match the parent build
# (e.g. CP0_ISOLATE_SYSROOT_HEADERS=OFF, a custom CM0_SDK_ROOT).
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES
    CP0_ISOLATE_SYSROOT_HEADERS
    CM0_SDK_ROOT
    CM0_SDK_VERSION
    CM0_SDK_URL
    CM0_ALLOW_FETCH_DEPS
    CM0_MULTIARCH
)
list(REMOVE_DUPLICATES CMAKE_TRY_COMPILE_PLATFORM_VARIABLES)

file(STRINGS "${CMAKE_SYSROOT}/usr/include/features.h" _CP0_GLIBC_MINOR_LINE
     REGEX "^#define[ \t]+__GLIBC_MINOR__[ \t]+[0-9]+")
if(_CP0_GLIBC_MINOR_LINE)
    string(REGEX REPLACE ".*__GLIBC_MINOR__[ \t]+([0-9]+).*" "\\1" CP0_SYSROOT_GLIBC_MINOR "${_CP0_GLIBC_MINOR_LINE}")
endif()

# Echo the compiler's default <...> search list for `lang` (c / c++) into `out`.
function(_cp0_default_include_dirs compiler lang out)
    execute_process(
        COMMAND "${compiler}" "--sysroot=${CMAKE_SYSROOT}" -E -Wp,-v -x ${lang} /dev/null
        OUTPUT_QUIET
        ERROR_VARIABLE _err
        RESULT_VARIABLE _rc
    )
    set(_dirs "")
    if(_rc EQUAL 0)
        string(REPLACE "\n" ";" _lines "${_err}")
        set(_in FALSE)
        foreach(_line IN LISTS _lines)
            if(_line MATCHES "^#include <\\.\\.\\.> search starts here:")
                set(_in TRUE)
            elseif(_line MATCHES "^End of search list\\.")
                set(_in FALSE)
            elseif(_in)
                string(STRIP "${_line}" _d)
                if(_d)
                    list(APPEND _dirs "${_d}")
                endif()
            endif()
        endforeach()
    endif()
    set(${out} "${_dirs}" PARENT_SCOPE)
endfunction()

# Build the list "-nostdinc;-isystem<dir>;..." for `lang`, keeping the default order minus any
# dir that is neither inside the sysroot nor the compiler's own (libstdc++ /
# gcc builtin) headers - i.e. the host libc headers.
function(_cp0_isolated_include_flags compiler lang out)
    _cp0_default_include_dirs("${compiler}" ${lang} _dirs)
    execute_process(COMMAND "${compiler}" -print-file-name=include
                    OUTPUT_VARIABLE _gcc_inc OUTPUT_STRIP_TRAILING_WHITESPACE)
    file(REAL_PATH "${_gcc_inc}" _gcc_inc)
    file(REAL_PATH "${CMAKE_SYSROOT}" _sysroot)
    set(_flags "")
    set(_dropped FALSE)
    set(_have_sysroot FALSE)
    foreach(_d IN LISTS _dirs)
        file(REAL_PATH "${_d}" _r)
        string(FIND "${_r}/" "${_sysroot}/" _pos)
        if(_pos EQUAL 0)
            set(_have_sysroot TRUE)
            list(APPEND _flags "-isystem${_d}")
        elseif(_r MATCHES "/c\\+\\+/" OR _r STREQUAL _gcc_inc OR _r STREQUAL "${_gcc_inc}-fixed")
            list(APPEND _flags "-isystem${_d}")
        else()
            set(_dropped TRUE)
            message(STATUS "cp0 toolchain: dropping host include dir for ${lang}: ${_d}")
        endif()
    endforeach()
    # Only rewrite when it fixes something and the sysroot dirs were found.
    if(_dropped AND _have_sysroot)
        set(${out} "-nostdinc;${_flags}" PARENT_SCOPE)
    else()
        set(${out} "" PARENT_SCOPE)
    endif()
endfunction()

# Apply on EVERY configure as directory compile options (inherited by all
# targets, FetchContent deps and try_compile projects), not via
# CMAKE_<LANG>_FLAGS_INIT: the INIT values only seed the cache on a fresh build
# dir, so an existing dir (or toggling CP0_ISOLATE_SYSROOT_HEADERS) would keep
# stale flags and silently compile against the host headers. User
# CMAKE_<LANG>_FLAGS are left untouched. The toolchain file is read more than
# once per configure, hence the marker check.
get_directory_property(_cp0_dir_defs COMPILE_DEFINITIONS)
if(NOT _cp0_dir_defs MATCHES "CP0_TOOLCHAIN_MANAGED_FLAGS")
    add_compile_definitions(CP0_TOOLCHAIN_MANAGED_FLAGS=1)
    if(DEFINED CP0_SYSROOT_GLIBC_MINOR)
        add_compile_definitions(CP0_SYSROOT_GLIBC_MINOR=${CP0_SYSROOT_GLIBC_MINOR})
    endif()
    if(CP0_ISOLATE_SYSROOT_HEADERS)
        _cp0_isolated_include_flags("${CMAKE_C_COMPILER}" c _CP0_C_INC_LIST)
        _cp0_isolated_include_flags("${CMAKE_CXX_COMPILER}" c++ _CP0_CXX_INC_LIST)
        foreach(_f IN LISTS _CP0_C_INC_LIST)
            add_compile_options("$<$<COMPILE_LANGUAGE:C>:${_f}>")
        endforeach()
        foreach(_f IN LISTS _CP0_CXX_INC_LIST)
            add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:${_f}>")
        endforeach()
    endif()
endif()
