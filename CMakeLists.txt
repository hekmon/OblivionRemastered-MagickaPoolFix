# MagickaPoolFix - CMake build, for local development and IDEs.
#
# The reference build is ./build.sh: it is what CI runs and what releases are made
# from. This file is the alternative for contributors who prefer CMake, an IDE, or
# MSVC on Windows. CI builds it too (MinGW-w64 on Linux, MSVC on Windows), so it
# cannot rot silently, but a release binary always comes from build.sh.
#
# It produces MagickaPoolFix.asi, a plain Win64 DLL with an .asi extension, loaded
# by an ASI loader. It is deliberately NOT an OBSE64 plugin: OBSE64 crashes on the
# maintainer's setup, and an ASI plugin has no dependency on a script extender.
# When Python 3 is found, the default MagickaPoolFix.conf is generated and checked
# next to it, as build.sh does.
#
#   MinGW-w64, cross-compiling from Linux or WSL (same flags as build.sh):
#       cmake -B build-cmake -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake \
#             -DCMAKE_BUILD_TYPE=Release
#       cmake --build build-cmake
#
#   MSVC, on Windows:
#       cmake -B build-cmake -A x64
#       cmake --build build-cmake --config Release
#
# The unit tests build for the host platform, not for Windows, because the
# headers they cover (MagickaMath.h, HypothesisCalibrator.h, Corrector.h)
# are free of any Windows dependency on purpose:
#       cmake -B build-tests -DMPF_BUILD_TESTS=ON -DMPF_BUILD_PLUGIN=OFF
#       cmake --build build-tests && ctest --test-dir build-tests --output-on-failure

cmake_minimum_required(VERSION 3.21)

# The version has one home, src/PCH.h. Read it from there.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/src/PCH.h" _mpf_version_line
     REGEX "^#define MPF_PLUGIN_VERSION ")
string(REGEX REPLACE "^#define MPF_PLUGIN_VERSION \"([0-9]+(\\.[0-9]+)*).*$" "\\1"
       MPF_VERSION "${_mpf_version_line}")
if(NOT MPF_VERSION MATCHES "^[0-9]+(\\.[0-9]+)*$")
    message(FATAL_ERROR "cannot read MPF_PLUGIN_VERSION from src/PCH.h")
endif()

project(MagickaPoolFix VERSION ${MPF_VERSION} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(MPF_BUILD_PLUGIN "Build the .asi plugin (requires a Windows target)" ON)
option(MPF_BUILD_TESTS  "Build the host-side unit tests"                    OFF)

# --------------------------------------------------------------------------
# The plugin
# --------------------------------------------------------------------------
if(MPF_BUILD_PLUGIN AND WIN32)
    add_library(MagickaPoolFix SHARED src/Plugin.cpp)

    target_include_directories(MagickaPoolFix PRIVATE src)

    # .asi, not .dll. ASI loaders only scan for this extension, and the user's
    # constraint is explicit: ASI plugins carry the .asi extension.
    set_target_properties(MagickaPoolFix PROPERTIES
        PREFIX ""
        SUFFIX ".asi")

    # No import library and no export table: an ASI loader only needs LoadLibrary.
    target_link_libraries(MagickaPoolFix PRIVATE psapi)

    if(MSVC)
        target_compile_definitions(MagickaPoolFix PRIVATE UNICODE _UNICODE)
        target_compile_options(MagickaPoolFix PRIVATE /W4 /permissive- /GR /EHsc)
        # Static CRT so the plugin does not need a redistributable installed.
        set_property(TARGET MagickaPoolFix PROPERTY
                     MSVC_RUNTIME_LIBRARY "MultiThreaded")
        target_compile_options(MagickaPoolFix PRIVATE $<$<CONFIG:Release>:/MT>)
    else()
        # Same flags as build.sh. -O2 comes after CMake's -O3 on the command line,
        # so it wins. -municode also defines UNICODE and _UNICODE.
        target_compile_options(MagickaPoolFix PRIVATE
            -O2 -municode -Wall -Wextra -Wpedantic)
        # Static libstdc++/libgcc for the same reason, strip, and hide every symbol.
        target_link_options(MagickaPoolFix PRIVATE
            -municode -s -static -static-libgcc -static-libstdc++
            -Wl,--exclude-all-symbols)
    endif()

    # The default .conf, generated from Config.h and checked, as build.sh does.
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_FOUND)
        add_custom_command(TARGET MagickaPoolFix POST_BUILD
            COMMAND Python3::Interpreter
                    "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_conf.py"
                    "${CMAKE_CURRENT_SOURCE_DIR}/src/Config.h"
                    "$<TARGET_FILE_DIR:MagickaPoolFix>/MagickaPoolFix.conf"
            VERBATIM)
    else()
        message(WARNING "Python 3 not found: MagickaPoolFix.conf will not be generated "
                        "(the plugin writes it on first launch anyway)")
    endif()
endif()

# --------------------------------------------------------------------------
# Unit tests (host platform)
# --------------------------------------------------------------------------
if(MPF_BUILD_TESTS)
    enable_testing()

    add_executable(test_math tests/test_math.cpp)
    target_include_directories(test_math PRIVATE src)
    add_test(NAME math COMMAND test_math)

    add_executable(test_calibration tests/test_calibration.cpp)
    target_include_directories(test_calibration PRIVATE src)
    add_test(NAME calibration COMMAND test_calibration)

    add_executable(test_corrector tests/test_corrector.cpp)
    target_include_directories(test_corrector PRIVATE src)
    add_test(NAME corrector COMMAND test_corrector)

    foreach(t test_math test_calibration test_corrector)
        if(MSVC)
            target_compile_options(${t} PRIVATE /W4)
        else()
            target_compile_options(${t} PRIVATE -Wall -Wextra -Wpedantic)
        endif()
    endforeach()
endif()
