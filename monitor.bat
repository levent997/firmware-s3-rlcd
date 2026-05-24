@echo off
REM Double-click to watch the device's serial log.
REM Press Ctrl+C (then Y) to quit.

setlocal
set PATH=%APPDATA%\Python\Python313\Scripts;%PATH%
cd /d "%~dp0"
pio device monitor --port COM3 --baud 115200
endlocal
pause
