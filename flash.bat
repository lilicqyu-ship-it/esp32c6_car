@echo off
rem ESP32-C6 firmware flash helper - activates the ESP-IDF environment, then
rem flashes this project.  No BOOT button needed (auto-download circuit).
rem
rem Usage:
rem   flash.bat              flash to default port (COM14)
rem   flash.bat COM7         flash to a specific port
rem   flash.bat COM14 mon    flash and open the serial monitor
setlocal
set "PORT=%~1"
if "%PORT%"=="" set "PORT=COM14"
set "EXTRA=%~2"
if /i "%EXTRA%"=="mon" set "EXTRA=monitor"

echo [c6] flashing to %PORT% ...
rem clear MSYSTEM if launched from Git Bash - idf.py refuses to run under MSys
set MSYSTEM=
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  ". 'C:\Espressif\tools\Microsoft.v6.1-beta1.PowerShell_profile.ps1'; Set-Location '%~dp0'; idf.py -p %PORT% flash %EXTRA%"
