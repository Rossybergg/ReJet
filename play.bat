@echo off
rem Run ReJet from the extracted disc in %USERPROFILE%\GameData\JSRF (tools\xiso.py extract-all).
rem Optional first argument: seconds to run (default 30; 0 = until the window is closed).
rem The log goes to build\loader\loader.log; set REJET_TRACE=1 to log every kernel call.
setlocal
set "SECS=%~1"
if "%SECS%"=="" set SECS=30
rem Run from build\loader so screenshots, captures and logs all land there (docs\TESTING.md).
cd /d "%~dp0build\loader"
.\ReJetLoader.exe "%USERPROFILE%\GameData\JSRF\default.xbe" %SECS%
