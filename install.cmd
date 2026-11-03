@echo off
rem Double-click to build and install the mod (installs Windhawk first if it's missing).
rem Runs deploy.ps1 with script execution allowed for this one run only.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0deploy.ps1" %*
echo.
pause
