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
# D3D: milestone 1 (device, Clear, Swap, resources). XPP and the DSOUND mixer from ReWhacked.
add_library(rejet SHARED
  core/loader.c
  core/emu_thread.c
  core/hle.c
  core/perf.c
  core/profile.c
  core/settings.c
  core/iso.c
  core/memdump.c
  core/widescreen.c
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
  core/kernel/xc.c
  core/d3d/d3d.c
  core/d3d/psh.c
  core/d3d/shadercache.c
  core/d3d/tex.c
  core/d3d/texconv.c
  core/d3d/vsh.c
  core/input/xpp.c
  core/audio/dsound.c
  core/ui/menu.c
  core/ui/hud.c
  core/ui/setup.c
  res/rejet.rc)
target_include_directories(rejet PRIVATE core ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_compile_options(rejet PRIVATE /W4 /arch:IA32)
target_link_libraries(rejet PRIVATE shell32 gdi32 comdlg32 windowscodecs d3d9 d3dcompiler xinput xaudio2 ole32 dbghelp psapi winmm dwmapi)
# Symbols for the profiler (REJET_PROFILE) and crash logs: rejet.pdb next to the dll, never
# packaged. /PDBALTPATH keeps the build machine's path out of the dll.
target_compile_options(rejet PRIVATE /Zi)
target_link_options(rejet PRIVATE /LARGEADDRESSAWARE /DEBUG /OPT:REF /OPT:ICF /PDBALTPATH:%_PDB%)

# ReJet.exe (target ReJetLoader): tiny stub that owns 0x10000.. (see ReWhacked's docs/LOADER.md).
add_executable(ReJetLoader stub/stub.c res/loader.rc)
target_include_directories(ReJetLoader PRIVATE core)
set_target_properties(ReJetLoader PROPERTIES OUTPUT_NAME ReJet) # players see ReJet.exe
target_compile_options(ReJetLoader PRIVATE /W4 /GS-)
target_link_libraries(ReJetLoader PRIVATE rejet kernel32)
target_link_options(ReJetLoader PRIVATE
  /NODEFAULTLIB /ENTRY:start /SUBSYSTEM:WINDOWS
  /BASE:0x10000 /FIXED /DYNAMICBASE:NO /SAFESEH:NO /LARGEADDRESSAWARE)

# rejet_tests.exe: unit tests of the pure parts (tests/README.md), built with the loader and run
# by tools/release_check.py. Same compiler flags as rejet.dll.
add_executable(rejet_tests
  ../tests/main.c
  ../tests/test_texconv.c
  ../tests/test_psh.c
  ../tests/test_widescreen.c
  ../tests/test_iso.c
  ../tests/test_settings.c
  core/d3d/texconv.c
  core/d3d/psh.c
  core/widescreen.c
  core/iso.c
  core/settings.c)
target_include_directories(rejet_tests PRIVATE core ../tests ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_compile_options(rejet_tests PRIVATE /W4 /arch:IA32)
