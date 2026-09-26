@echo off
rem ESP32-C6 flashing helper - activates the ESP-IDF environment, then flashes
rem this project.  No BOOT button needed (auto-download circuit).
rem
rem Usage:  flash.bat [mode] [COM port] [mon]
rem   mode   full (default) = bootloader + partition table + otadata + firmware
rem          assets         = control page only (repacks assets_src, firmware untouched)
rem          all            = assets first, then full firmware (one power cycle for the user)
rem   COM    default COM14 (e.g. flash.bat COM7)
rem   mon    open the serial monitor after flashing (full/all only)
setlocal
set "MODE=full"
set "PORT=COM14"
set "EXTRA="
:parse
if "%~1"=="" goto run
if /i "%~1"=="full" (set "MODE=full" & shift & goto parse)
if /i "%~1"=="assets" (set "MODE=assets" & shift & goto parse)
if /i "%~1"=="all" (set "MODE=all" & shift & goto parse)
if /i "%~1"=="mon" (set "EXTRA=monitor" & shift & goto parse)
(set "PORT=%~1" & shift & goto parse)
:run
echo [c6] mode=%MODE% port=%PORT% ...
rem clear MSYSTEM if launched from Git Bash - idf.py refuses to run under MSys
set MSYSTEM=
powershell -NoProfile -ExecutionPolicy Bypass -Command ". 'C:\Espressif\tools\Microsoft.v6.1-beta1.PowerShell_profile.ps1'; Set-Location '%~dp0'; $m='%MODE%'; $p='%PORT%'; if ($m -eq 'assets' -or $m -eq 'all') { python tools/build_assets.py assets_src build/assets.bin; parttool.py -p $p write_partition --partition-name=assets --input build/assets.bin }; if ($m -eq 'full' -or $m -eq 'all') { idf.py -p $p flash %EXTRA% }"
