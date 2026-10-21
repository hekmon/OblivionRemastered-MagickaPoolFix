# MagickaPoolFix - build script
#
# Produces MagickaPoolFix.asi, a plain Win64 DLL with an .asi extension, loaded by
# an ASI loader. It is deliberately NOT an OBSE64 plugin: OBSE64 is reported as
# unstable on this setup, and an ASI plugin has no dependency on the script
# extender at all.
#
# Two toolchains are supported and produce a functionally identical binary:
#
#   MSVC (Windows, recommended if you have Visual Studio):
#       cmake -B build -A x64
#       cmake --build build --config Release
#
#   MinGW-w64 (cross-compiling from Linux, which is how the shipped binary was
#   built):
#       cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake \
#             -DCMAKE_BUILD_TYPE=Release
#       cmake --build build
#
# The unit tests build for the host platform, not for Windows, because the
# headers they cover (MagickaMath.h, HypothesisCalibrator.h, Corrector.h)
# are free of any Windows dependency on purpose:
#       cmake -B build-tests -DMPF_BUILD_TESTS=ON
#       cmake --build build-tests && ctest --test-dir build-tests --output-on-failure

cmake_minimum_required(VERSION 3.21)
project(MagickaPoolFix VERSION 0.9.7 LANGUAGES CXX)

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
        target_compile_options(MagickaPoolFix PRIVATE /W4 /permissive- /GR /EHsc)
        # Static CRT so the plugin does not need a redistributable installed.
        set_property(TARGET MagickaPoolFix PROPERTY
                     MSVC_RUNTIME_LIBRARY "MultiThreaded")
        target_compile_options(MagickaPoolFix PRIVATE $<$<CONFIG:Release>:/MT>)
    else()
        target_compile_options(MagickaPoolFix PRIVATE -Wall -Wextra -Wpedantic)
        # Static libstdc++/libgcc for the same reason, and hide every symbol.
        target_link_options(MagickaPoolFix PRIVATE
            -static -static-libgcc -static-libstdc++
            -Wl,--exclude-all-symbols)
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
