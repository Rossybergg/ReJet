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
  core/kernel/xc.c
  core/d3d/d3d.c
  core/d3d/psh.c
  core/d3d/shadercache.c
  core/d3d/tex.c
  core/d3d/vsh.c
  core/input/xpp.c
  core/audio/dsound.c
  core/ui/menu.c
  core/ui/setup.c
  res/rejet.rc)
target_include_directories(rejet PRIVATE core ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_compile_options(rejet PRIVATE /W4 /arch:IA32)
target_link_libraries(rejet PRIVATE shell32 gdi32 comdlg32 windowscodecs d3d9 d3dcompiler xinput xaudio2 ole32)
target_link_options(rejet PRIVATE /LARGEADDRESSAWARE)

# ReJet.exe (target ReJetLoader): tiny stub that owns 0x10000.. (see ReWhacked's docs/LOADER.md).
add_executable(ReJetLoader stub/stub.c res/loader.rc)
target_include_directories(ReJetLoader PRIVATE core)
set_target_properties(ReJetLoader PROPERTIES OUTPUT_NAME ReJet) # players see ReJet.exe
target_compile_options(ReJetLoader PRIVATE /W4 /GS-)
target_link_libraries(ReJetLoader PRIVATE rejet kernel32)
target_link_options(ReJetLoader PRIVATE
  /NODEFAULTLIB /ENTRY:start /SUBSYSTEM:WINDOWS
  /BASE:0x10000 /FIXED /DYNAMICBASE:NO /SAFESEH:NO /LARGEADDRESSAWARE)
