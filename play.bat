@echo off
rem Run ReJet from the build (build\loader\ReJet.exe). Without arguments it uses the saved game
rem folder ([Game] Folder in rejet.ini), else Game\ next to it, else the first-run setup.
rem Developers: play.bat <path to default.xbe> [seconds to run, 0 = until the window is closed].
rem The log, screenshots and captures go to build\loader (docs\TESTING.md).
cd /d "%~dp0build\loader"
.\ReJet.exe %*
