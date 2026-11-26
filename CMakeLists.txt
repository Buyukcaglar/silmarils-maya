# Le Fetiche Maya 64bit - a static recompilation of the Amiga game's interpreter T.X (docs/RECOMP-PLAN.md).
#
# The game's code is not in this repository: the build generates C from the user's own copy of T.X
# (tools/m68k/gen_c.py) into the build folder.  Point MAYA_DATA at the Amiga game's folder (the one with T.X and
# MAIN.CO):
#
#   cmake -G Ninja -B build/win -DCMAKE_BUILD_TYPE=Release -DMAYA_DATA=D:/Games/Maya/data
#   cmake --build build/win
#
# Outputs: maya64.exe (the Windows host) and maya_cli.exe (the headless test runner).
cmake_minimum_required(VERSION 3.20)
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
endif()
