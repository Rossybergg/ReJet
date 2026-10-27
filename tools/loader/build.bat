@echo off
rem Build the ReJet loader (x86, Release) into build\loader.
rem Usage: tools\loader\build.bat
setlocal
set "VS=C:\Program Files\Microsoft Visual Studio\18\Community"
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "%VS%\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1
set "PATH=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
cd /d "%~dp0..\.."
python -I tools\loader\gen_kernel.py || exit /b 1
python -I tools\loader\gen_xdk_addrs.py || exit /b 1
rem fs_patches.gen.c needs orig\Default.xbe; it is committed, so only regenerate when present.
if exist orig\Default.xbe python -I tools\loader\gen_fs_patches.py || exit /b 1
cmake -S loader -B build\loader -G Ninja -DCMAKE_BUILD_TYPE=Release || exit /b 1
cmake --build build\loader || exit /b 1
