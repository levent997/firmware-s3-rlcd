@echo off
REM Build and flash to COM3. Close the monitor window first (it locks the port).

setlocal
set PATH=%APPDATA%\Python\Python313\Scripts;%PATH%
cd /d "%~dp0"
pio run --upload-port COM3 -t upload
endlocal
pause
