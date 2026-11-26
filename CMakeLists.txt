# Le Fetiche Maya 64bit - a static recompilation of the Amiga game's interpreter T.X (docs/RECOMP-PLAN.md).
#
# The game's code is not in this repository: the build generates C from the user's own copy of T.X
# (tools/m68k/gen_c.py) into the build folder.  Point MAYA_DATA at the Amiga game's folder (the one with T.X and
# MAIN.CO):
#
#   cmake -G Ninja -B build/win -DCMAKE_BUILD_TYPE=Release -DMAYA_DATA=D:/Games/Maya/data
#   cmake --build build/win
#
# Outputs: maya64.exe (the Windows host) and maya_cli.exe (the headless test runner); on macOS
# "Le Fetiche Maya 64bit.app" (universal: arm64 + x86_64) and maya_cli (build.sh, docs/MACOS-PORT.md).
cmake_minimum_required(VERSION 3.20)
if(APPLE)
    # one universal program for Apple silicon and Intel Macs (set before project() so they apply)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "the architectures of the macOS build")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "the oldest macOS the build runs on")
endif()
project(Maya C)
if(WIN32)
    enable_language(RC)
endif()

set(MAYA_DATA "${CMAKE_SOURCE_DIR}/_extracted/amiga/Maya/data" CACHE PATH "The Amiga game's folder (T.X, MAIN.CO)")
find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)

set(MAYA_TX "${MAYA_DATA}/T.X")
if(NOT EXISTS "${MAYA_TX}")
    message(FATAL_ERROR "T.X not found in MAYA_DATA (${MAYA_DATA}). Set -DMAYA_DATA=<the Amiga game's folder>.")
endif()

# the recompiled game
set(GEN "${CMAKE_BINARY_DIR}/gen")
file(GLOB M68K_TOOLS CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/tools/m68k/*.py")
add_custom_command(
    OUTPUT "${GEN}/tx_code.c" "${GEN}/tx_image.c" "${GEN}/tx_gen.h"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${GEN}"
    COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_SOURCE_DIR}/tools/m68k/gen_c.py" "${MAYA_TX}" "${GEN}"
    DEPENDS ${M68K_TOOLS} "${MAYA_TX}"
    COMMENT "Recompiling T.X to C"
    VERBATIM)

if(MSVC)
    add_compile_options(/W3 /utf-8 /wd4244 /wd4267 /wd4146)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
    if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
        add_compile_options(-Wno-unused-parameter)
    endif()
else()
    add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

add_library(maya_runtime STATIC
    runtime/machine.c runtime/custom.c runtime/blitter.c runtime/paula.c runtime/cia.c runtime/hle.c
    runtime/hooks.c "${GEN}/tx_code.c" "${GEN}/tx_image.c")
target_include_directories(maya_runtime PUBLIC runtime "${GEN}")
set_source_files_properties("${GEN}/tx_code.c" PROPERTIES COMPILE_OPTIONS "$<$<C_COMPILER_ID:MSVC>:/bigobj>")

add_library(maya_present STATIC present/layout.c present/crt.c)
target_include_directories(maya_present PUBLIC present)
if(NOT MSVC)
    target_link_libraries(maya_present PUBLIC m)
endif()

add_executable(maya_cli tests/maya_cli.c)
target_link_libraries(maya_cli PRIVATE maya_runtime)

if(WIN32)
    add_executable(maya64 WIN32 host/win32/maya_win32.c host/win32/maya64.rc)
    target_link_libraries(maya64 PRIVATE maya_runtime maya_present
        user32 gdi32 shell32 ole32 uuid dwmapi uxtheme winmm advapi32)
    set_source_files_properties(host/win32/maya64.rc PROPERTIES
        OBJECT_DEPENDS "${CMAKE_SOURCE_DIR}/res/maya64.ico;${CMAKE_SOURCE_DIR}/host/win32/maya64.manifest;${CMAKE_SOURCE_DIR}/host/win32/host_config.h")
    if(MSVC)
        # the manifest comes from the .rc
        target_link_options(maya64 PRIVATE /MANIFEST:NO)
    endif()
elseif(APPLE)
    enable_language(OBJC)
    set(MAYA_VERSION "1.0.0")      # also HOST_VERSION_STR in host/macos/host_config.h

    # the app icon: res/macos/maya64-icon-macos.png (tools/make_mac_icon.m) at every size an .icns holds
    set(MAYA_ICONSET "${CMAKE_BINARY_DIR}/maya64.iconset")
    set(MAYA_ICNS "${CMAKE_BINARY_DIR}/maya64.icns")
    set(MAYA_ICON_PNG "${CMAKE_SOURCE_DIR}/res/macos/maya64-icon-macos.png")
    set(_icon_cmds)
    foreach(sz 16 32 128 256 512)
        math(EXPR sz2 "${sz} * 2")
        list(APPEND _icon_cmds
            COMMAND sips -z ${sz} ${sz} "${MAYA_ICON_PNG}" --out "${MAYA_ICONSET}/icon_${sz}x${sz}.png"
            COMMAND sips -z ${sz2} ${sz2} "${MAYA_ICON_PNG}" --out "${MAYA_ICONSET}/icon_${sz}x${sz}@2x.png")
    endforeach()
    add_custom_command(OUTPUT "${MAYA_ICNS}"
        COMMAND "${CMAKE_COMMAND}" -E rm -rf "${MAYA_ICONSET}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${MAYA_ICONSET}"
        ${_icon_cmds}
        COMMAND iconutil -c icns "${MAYA_ICONSET}" -o "${MAYA_ICNS}"
        DEPENDS "${MAYA_ICON_PNG}"
        COMMENT "Making the app icon"
        VERBATIM)
    set_source_files_properties("${MAYA_ICNS}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)

    add_executable(maya64 MACOSX_BUNDLE host/macos/maya_macos.m "${MAYA_ICNS}")
    set_target_properties(maya64 PROPERTIES
        OUTPUT_NAME "Le Fetiche Maya 64bit"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/host/macos/Info.plist.in"
        MACOSX_BUNDLE_GUI_IDENTIFIER com.buyukcaglar.maya64
        MACOSX_BUNDLE_BUNDLE_VERSION ${MAYA_VERSION}
        MACOSX_BUNDLE_SHORT_VERSION_STRING ${MAYA_VERSION}
        MACOSX_BUNDLE_ICON_FILE maya64.icns)
    set_source_files_properties(host/macos/maya_macos.m PROPERTIES
        COMPILE_OPTIONS "-fobjc-arc"
        OBJECT_DEPENDS "${CMAKE_SOURCE_DIR}/host/macos/host_config.h")
    target_link_libraries(maya64 PRIVATE maya_runtime maya_present
        "-framework Cocoa" "-framework QuartzCore" "-framework AudioToolbox" "-framework GameController")
    # development checks of the app (scripted input inside its own process, snapshots): never in a release
    option(MAYA_DEVTEST "build the app's development checks (host/macos/maya_devtest.inc)" OFF)
    if(MAYA_DEVTEST)
        target_compile_definitions(maya64 PRIVATE MAYA_DEVTEST)
    endif()

    # the signature: ad hoc by default; with a Developer ID (-DMAYA_CODESIGN_IDENTITY="Developer ID Application: ...")
    # the hardened runtime and a secure timestamp, as notarization requires (tools/package_mac.sh)
    set(MAYA_CODESIGN_IDENTITY "-" CACHE STRING "codesign identity for the app (- signs ad hoc)")
    if(MAYA_CODESIGN_IDENTITY STREQUAL "-")
        set(_sign_opts --timestamp=none)
    else()
        set(_sign_opts --options runtime --timestamp)
    endif()
    add_custom_command(TARGET maya64 POST_BUILD
        COMMAND codesign --force --sign "${MAYA_CODESIGN_IDENTITY}" ${_sign_opts} "$<TARGET_BUNDLE_DIR:maya64>"
        COMMENT "Signing the app (${MAYA_CODESIGN_IDENTITY})"
        VERBATIM)
endif()
