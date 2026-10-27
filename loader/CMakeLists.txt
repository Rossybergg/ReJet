cmake_minimum_required(VERSION 3.24)
project(rejet_loader C)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "rejet must be built 32-bit (x86)")
endif()

set(CMAKE_C_STANDARD 17)
add_compile_definitions(_CRT_SECURE_NO_WARNINGS)

# Static CRT so the loader has no runtime DLL dependencies.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# rejet.dll: all loader logic. x87 floating point for anything game-facing (CLAUDE.md).
# Phase 2: boot path only. The D3D, DSOUND and XPP shims come over from ReWhacked in Phase 3.
add_library(rejet SHARED
  core/loader.c
  core/emu_thread.c
  core/hle.c
  core/perf.c
  core/settings.c
  core/fs_patches.gen.c
  core/kernel_imports.gen.c
  core/kernel/common.c
  core/kernel/dpc.c
  core/kernel/hal.c
  core/kernel/io.c
  core/kernel/ke.c
  core/kernel/mm.c
  core/kernel/ps.c
  core/kernel/rtl.c
  core/kernel/sync.c
  core/kernel/xc.c)
target_include_directories(rejet PRIVATE core ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_compile_options(rejet PRIVATE /W4 /arch:IA32)
target_link_libraries(rejet PRIVATE shell32)
target_link_options(rejet PRIVATE /LARGEADDRESSAWARE)

# ReJetLoader.exe: tiny stub that owns 0x10000.. (see ReWhacked's docs/LOADER.md).
add_executable(ReJetLoader stub/stub.c)
target_compile_options(ReJetLoader PRIVATE /W4 /GS-)
target_link_libraries(ReJetLoader PRIVATE rejet kernel32)
target_link_options(ReJetLoader PRIVATE
  /NODEFAULTLIB /ENTRY:start /SUBSYSTEM:WINDOWS
  /BASE:0x10000 /FIXED /DYNAMICBASE:NO /SAFESEH:NO /LARGEADDRESSAWARE)
