@echo off
rem Double-click builder: just forwards to build.ps1 (the real script).
rem Pass -DlssnrDll to point at the NR runtime if it is not already in payload\.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
pause
