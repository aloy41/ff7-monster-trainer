@echo off
rem FF7: Monster Trainer - double-click to install. Arguments are passed to Setup.ps1 (-GameDir, -Target).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Setup.ps1" %*
pause
