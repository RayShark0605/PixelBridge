@echo off
setlocal
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0Start-G21Encoder.ps1" -Stage smoke
set "encoderExit=%errorlevel%"
echo.
echo Script exit: %encoderExit%
pause
exit /b %encoderExit%
