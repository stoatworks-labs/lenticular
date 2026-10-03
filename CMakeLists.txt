cmake_minimum_required(VERSION 3.15)

project(Lenticular
    VERSION 0.1.0
    LANGUAGES CXX
)

# ---------------------------------------------------------------------------
# Universal by default so one bundle loads in both the Apple Silicon and the
# Intel build of Resolume. Override with -DCMAKE_OSX_ARCHITECTURES=arm64 for a
# faster dev build.
#
# This must sit immediately after project() and before any target exists. CMake
# latches the architecture list when the first target is created; setting it
# later is silently ignored and you get an arm64-only binary that the build log
# calls a success. Verify with `lipo`, never with the log.
# ---------------------------------------------------------------------------
if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Apple deprecated the whole OpenGL API in 10.14 and FFGL's plugin interface is
# still OpenGL, so every SDK call warns. There is no alternative API to move to
# here, so silencing it is not ignoring a problem.
if(APPLE)
    add_compile_definitions(GL_SILENCE_DEPRECATION)
endif()

if(MSVC)
    add_compile_options(/utf-8)
endif()

# windows.h defines min and max as macros, which turns every std::min into
# `std::(...)` and fails with an error naming neither min nor windows.h.
if(WIN32)
    add_compile_definitions(NOMINMAX)
endif()

option(LENTICULAR_BUILD_TOOLS "Build the offline render and measurement harness" ON)

# The FFGL plugin, the FFGL SDK submodule and GLEW are one dependency group, and
# the OpenFX plugin belongs to none of it. Turning this OFF builds the OFX plugin
# alone, with no GL loader anywhere in the configure -- which is what the Linux
# job does, because the FFGL SDK has no Linux path and GLEW would otherwise have
# to be installed to build something that never calls it.
option(LENTICULAR_BUILD_FFGL "Build the FFGL plugin (needs the FFGL SDK and, off macOS, GLEW)" ON)

# ---------------------------------------------------------------------------
# The parts with no GL in them at all: the parameter conversions, the lens
# setup, the uniforms and the CPU twin of the card shader (Card.h).
#
# A separate library from the FFGL core because three builds need it and only
# one of them has a GL context -- the OpenFX plugin renders on the CPU and the
# Linux job has no GL loader configured. Splitting it is what lets the OpenFX
# build link the SAME arithmetic the FFGL build hands its shader, rather than a
# second transcription of it.
# ---------------------------------------------------------------------------
add_library(lenticular_card OBJECT
    source/Controls.h
    source/Controls.cpp
    source/Lens.h
    source/Card.h
    source/Card.cpp
)
target_include_directories(lenticular_card PUBLIC source)
set_target_properties(lenticular_card PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(LENTICULAR_BUILD_FFGL)

# ---------------------------------------------------------------------------
# The Resolume FFGL SDK, as a git submodule pinned to b1afaf9 -- the rev the
# rest of the fleet pins.
# ---------------------------------------------------------------------------
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/ffgl/CMakeLists.txt")
    message(FATAL_ERROR
        "FFGL SDK not found. Run: git submodule update --init --recursive")
endif()

set(FFGL_BUILD_EXAMPLE_PLUGINS OFF CACHE BOOL "" FORCE)
add_subdirectory(external/ffgl EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# Everything the plugin is, as an OBJECT library so both the bundle and the
# offline harness are built from exactly the same code.
#
# OBJECT and not STATIC on purpose. `CFFGLPluginInfo` registers itself from a
# file-scope constructor in Lenticular.cpp and nothing ever references that object
# by name, so in an archive the linker is entitled to drop the whole
# translation unit -- and the result is a bundle that loads, exports plugMain,
# and reports that it contains no plugins.
# ---------------------------------------------------------------------------
add_library(lenticular_core OBJECT
    source/StoatworksAbout.h
    source/StoatworksAboutLinks.h
    source/StoatworksAboutParams.h
    source/Diag.h
    source/Diag.cpp
    source/Shaders.h
    source/Shaders.cpp
    source/Lenticular.h
    source/Lenticular.cpp
)

target_include_directories(lenticular_core PUBLIC source)
target_link_libraries(lenticular_core PUBLIC ffgl::sdk lenticular_card)
target_compile_definitions(lenticular_core PUBLIC LENTICULAR_VERSION="${PROJECT_VERSION}")
set_target_properties(lenticular_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

# On Windows/Linux the SDK headers pull in <GL/glew.h>. The SDK links GLEW
# privately, so consumers need it too -- for the include path and for the
# GLEW_STATIC define the static vcpkg triplet propagates. macOS uses the system
# OpenGL framework and needs none of this. GLEW arrives through vcpkg.json.
if(NOT APPLE)
    find_package(GLEW REQUIRED)
    target_link_libraries(lenticular_core PUBLIC GLEW::GLEW)
endif()

# ---------------------------------------------------------------------------
# The plugin. FFGL resolves one plugMain per binary.
# ---------------------------------------------------------------------------
function(lenticular_add_plugin target entry bundle_name identifier)
    add_library(${target} MODULE ${entry})
    # lenticular_card is named again here, and on lntest, because an OBJECT
    # library's objects do NOT travel transitively through a second OBJECT
    # library: lenticular_core linking it gives this target the include path
    # and nothing to link, and the failure is a pile of undefined symbols at
    # the very last step of an otherwise clean build.
    target_link_libraries(${target} PRIVATE lenticular_core lenticular_card)

    if(APPLE)
        set_target_properties(${target} PROPERTIES
            BUNDLE TRUE
            BUNDLE_EXTENSION "bundle"
            OUTPUT_NAME "${bundle_name}"
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Info.plist.in"
            MACOSX_BUNDLE_BUNDLE_NAME "${bundle_name}"
            MACOSX_BUNDLE_GUI_IDENTIFIER "${identifier}"
            MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        )
        # FFGL hosts resolve plugMain by symbol name, so keep default visibility.
        target_link_libraries(${target} PRIVATE
            "-framework OpenGL"
            "-framework Carbon"
            "-framework AppKit"
        )
    else()
        set_target_properties(${target} PROPERTIES OUTPUT_NAME "${bundle_name}")
    endif()
endfunction()

lenticular_add_plugin(Lenticular source/PluginEntry.cpp
    "Lenticular" "com.stoatworks.ffgl.lenticular")

# `cmake --install build` drops the bundle where Arena reads it. Avenue reads
# ~/Documents/Resolume Avenue/Extra Effects instead; pass --prefix for that.
#
# A mixer goes in Extra Effects like every other FFGL plugin. The fleet's
# first mixers said "Extra Mixers" until genlock's release, on the
# assumption that Resolume keeps mixers apart; it does not. Measured on
# genlock in Resolume Arena 7.27.1 (2026-09-23): Arena scans Extra Effects,
# loads the mixer from there and offers it as a layer Blend Mode, and its
# binary names no "Extra Mixers" anywhere.
if(APPLE AND CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "$ENV{HOME}/Documents/Resolume Arena/Extra Effects"
        CACHE PATH "" FORCE)
endif()
install(TARGETS Lenticular
    LIBRARY DESTINATION "."
    BUNDLE  DESTINATION "."
)
# The OpenFX bundle is deliberately NOT installed here: it belongs in
# /Library/OFX/Plugins (root-owned), not in Resolume's folder. See the README.

# ---------------------------------------------------------------------------
# The harness: the real plugin class in a real GL context, with no host.
#
# macOS only, because it stands up a CGL context directly. It feeds TWO
# inputs, because this is a mixer (the fleet's fourth) -- see
# tools/lntest/main.cpp.
# ---------------------------------------------------------------------------
if(LENTICULAR_BUILD_TOOLS AND APPLE)
    enable_testing()

    add_executable(lntest tools/lntest/main.cpp)
    # zlib ships with the OS, which is why the PNG writer is fifty lines
    # rather than a vendored dependency.
    target_link_libraries(lntest PRIVATE lenticular_core lenticular_card z
        "-framework OpenGL"
        "-framework AppKit"
    )

    add_test(NAME names    COMMAND lntest --names)
    add_test(NAME mixer    COMMAND lntest --mixer)
    add_test(NAME ends     COMMAND lntest --ends)
    add_test(NAME flip     COMMAND lntest --flip)
    add_test(NAME moire    COMMAND lntest --moire)
    add_test(NAME distance COMMAND lntest --distance)
    add_test(NAME opacity  COMMAND lntest --opacity)
    add_test(NAME mutation COMMAND lntest --mutation)
    add_test(NAME cpu      COMMAND lntest --cpu)
    add_test(NAME fade     COMMAND lntest --fade)
endif()

endif()  # LENTICULAR_BUILD_FFGL

# ---------------------------------------------------------------------------
# The OpenFX build, for Resolve, Vegas and any other host with a Transition
# context. CPU render against the vendored OFX SDK subset under
# external/openfx (C headers + the official C++ Support library, BSD-3).
# lenticular_card links straight in, so the parameter conversions, the lens
# setup and the uniforms are the same code the FFGL build hands its shader;
# only the shader's per-pixel stage is written twice, in Card.cpp, and
# `lntest --cpu` holds the two together.
# ---------------------------------------------------------------------------
option(BUILD_OFX "Build the OpenFX plugin" ON)

if(BUILD_OFX)
    set(OFX_SDK "${CMAKE_CURRENT_SOURCE_DIR}/external/openfx")

    add_library(LenticularOFX MODULE
        source/ofx/LenticularOFX.cpp
        ${OFX_SDK}/Support/Library/ofxsCore.cpp
        ${OFX_SDK}/Support/Library/ofxsImageEffect.cpp
        ${OFX_SDK}/Support/Library/ofxsInteract.cpp
        ${OFX_SDK}/Support/Library/ofxsLog.cpp
        ${OFX_SDK}/Support/Library/ofxsMultiThread.cpp
        ${OFX_SDK}/Support/Library/ofxsParams.cpp
        ${OFX_SDK}/Support/Library/ofxsProperty.cpp
        ${OFX_SDK}/Support/Library/ofxsPropertyValidation.cpp
    )
    # PRIVATE on an OBJECT library gives both the objects and the usage
    # requirements. `$<TARGET_OBJECTS:lenticular_card>` would give the objects
    # alone -- no include directories -- and the failure is platform-specific
    # enough to cost a day.
    target_link_libraries(LenticularOFX PRIVATE lenticular_card)
    target_include_directories(LenticularOFX PRIVATE
        source
        "${OFX_SDK}/include"
        "${OFX_SDK}/Support/include"
    )
    target_compile_definitions(LenticularOFX PRIVATE
        LENTICULAR_VERSION="${PROJECT_VERSION}"
        PLUGIN_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
        PLUGIN_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    )

    # Threads, explicitly. The OFX Support library's multi-thread suite calls
    # pthread_create, and on Rocky 8 -- glibc 2.28, the floor Resolve supports --
    # that still lives in libpthread; libpthread was only folded into libc in
    # glibc 2.34. Leave it out and the plugin compiles, links, passes a symbol
    # check and a glibc-version check, and then fails at dlopen with "undefined
    # symbol: pthread_create". macOS and Windows never show it.
    find_package(Threads REQUIRED)
    target_link_libraries(LenticularOFX PRIVATE Threads::Threads)

    set(OFX_BUNDLE_DIR "${CMAKE_BINARY_DIR}/Lenticular.ofx.bundle")
    set_target_properties(LenticularOFX PROPERTIES
        PREFIX ""
        SUFFIX ".ofx"
        OUTPUT_NAME "Lenticular"
        POSITION_INDEPENDENT_CODE ON
        LIBRARY_OUTPUT_DIRECTORY "${OFX_BUNDLE_DIR}/Contents/MacOS"
    )
    if(APPLE)
        set(OFX_BUNDLE_IDENTIFIER "com.stoatworks.lenticular.ofx")
        configure_file(cmake/InfoOFX.plist.in
            "${OFX_BUNDLE_DIR}/Contents/Info.plist" @ONLY)
    elseif(WIN32)
        set_target_properties(LenticularOFX PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY "${OFX_BUNDLE_DIR}/Contents/Win64"
            RUNTIME_OUTPUT_DIRECTORY "${OFX_BUNDLE_DIR}/Contents/Win64"
            LIBRARY_OUTPUT_DIRECTORY_RELEASE "${OFX_BUNDLE_DIR}/Contents/Win64"
            RUNTIME_OUTPUT_DIRECTORY_RELEASE "${OFX_BUNDLE_DIR}/Contents/Win64"
            # MSVC's .exp/.lib for this target; its default directory collides
            # with the FFGL target's, which shares the output base name, and a
            # parallel build then races on the file (LNK1104).
            ARCHIVE_OUTPUT_DIRECTORY "${OFX_BUNDLE_DIR}/importlib"
            ARCHIVE_OUTPUT_DIRECTORY_RELEASE "${OFX_BUNDLE_DIR}/importlib")
    else()
        set_target_properties(LenticularOFX PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY "${OFX_BUNDLE_DIR}/Contents/Linux-x86-64")
    endif()
endif()
